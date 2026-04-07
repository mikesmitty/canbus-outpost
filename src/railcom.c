#include "railcom.h"
#include "board.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "railcom_sample.pio.h"
#include "pico/multicore.h"
#include <string.h>
#include "util/dbg.h"

// ---------------------------------------------------------------------------
// RailCom 4/8 decode table (RCN-217)
// ---------------------------------------------------------------------------
static const uint8_t decode_table[256] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x33, 0xFF, 0xFF, 0xFF, 0x34, 0xFF, 0x35, 0x36, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3A, 0xFF, 0xFF, 0xFF, 0x3B, 0xFF, 0x3C, 0x37, 0xFF,
    0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0x3D, 0x38, 0xFF, 0xFF, 0x3E, 0x39, 0xFF, 0xFD, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x24, 0xFF, 0xFF, 0xFF, 0x23, 0xFF, 0x22, 0x21, 0xFF,
    0xFF, 0xFF, 0xFF, 0x1F, 0xFF, 0x1E, 0x20, 0xFF, 0xFF, 0x1D, 0x1C, 0xFF, 0x1B, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x19, 0xFF, 0x18, 0x1A, 0xFF, 0xFF, 0x17, 0x16, 0xFF, 0x15, 0xFF, 0xFF, 0xFF,
    0xFF, 0x25, 0x14, 0xFF, 0x13, 0xFF, 0xFF, 0xFF, 0x32, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFA, 0xFF, 0xFF, 0xFF, 0x0E, 0xFF, 0x0D, 0x0C, 0xFF,
    0xFF, 0xFF, 0xFF, 0x0A, 0xFF, 0x09, 0x0B, 0xFF, 0xFF, 0x08, 0x07, 0xFF, 0x06, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x04, 0xFF, 0x03, 0x05, 0xFF, 0xFF, 0x02, 0x01, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0x0F, 0x10, 0xFF, 0x11, 0xFF, 0xFF, 0xFF, 0x12, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFB, 0xFF, 0x2B, 0x30, 0xFF, 0xFF, 0x2A, 0x2F, 0xFF, 0x31, 0xFF, 0xFF, 0xFF,
    0xFF, 0x29, 0x2E, 0xFF, 0x2D, 0xFF, 0xFF, 0xFF, 0x2C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFC, 0x28, 0xFF, 0x27, 0xFF, 0xFF, 0xFF, 0x26, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define SAMPLE_RATE_HZ      2000000
#define SAMPLES_PER_BIT     (SAMPLE_RATE_HZ / 250000)  // 8

// Cutout detection: A != B for >15µs = 30 samples at 2MHz
#define SILENCE_THRESHOLD   30

// Cutout window: process samples for 450µs after cutout confirmation
// (~465µs from actual cutout start, within the ~488µs cutout)
#define CUTOUT_WINDOW       900  // samples at 2MHz

// Ring buffer: 4KB = 1024 uint32_t words = ~2ms of sample data
#define RING_BITS           12
#define RING_SIZE           (1u << RING_BITS)
#define RING_WORDS          (RING_SIZE / sizeof(uint32_t))
#define RING_MASK           (RING_WORDS - 1)

// DMA restarts after this many transfers (~143 minutes at 2MHz)
#define DMA_XFER_COUNT      (0xFFFFFC00u)  // ~4 billion, aligned to RING_WORDS

#define RC_MAX_BYTES        12  // max bytes per cutout per pin

// ---------------------------------------------------------------------------
// Software UART types
// ---------------------------------------------------------------------------
typedef enum {
    UART_IDLE,
    UART_START_BIT,
    UART_DATA_BITS,
    UART_STOP_BIT,
} uart_state_t;

typedef struct {
    uart_state_t state;
    uint8_t sample_count;
    uint8_t bit_count;
    uint8_t shift_reg;
    bool last_val;
} soft_uart_t;

// ---------------------------------------------------------------------------
// Per-channel decoder state (lives on core 1)
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t silence_count;
    bool in_cutout;
    uint16_t cutout_count;
    soft_uart_t uart_nand;  // Capture A=1, B=1 pulses (!(A && B) logic)
    soft_uart_t uart_or;    // Capture A=0, B=0 pulses (A || B logic)
    uint8_t rx_buf_nand[RC_MAX_BYTES];
    uint8_t rx_buf_or[RC_MAX_BYTES];
    uint8_t rx_count_nand;
    uint8_t rx_count_or;
} rc_decoder_t;

// ---------------------------------------------------------------------------
// Per-channel state on core 0
// ---------------------------------------------------------------------------
typedef struct {
    railcom_data_t last_nand;
    railcom_data_t last_or;
    bool signal_active;
    absolute_time_t last_valid_cutout;
    bool last_nand_valid;   // True if last data was from NAND decoder (orientation)
} rc_channel_t;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static uint32_t __attribute__((aligned(RING_SIZE))) ring_buf[RING_WORDS];

static PIO sample_pio;
static uint sample_sm;
static uint dma_chan;
static dma_channel_config dma_cfg;

static rc_decoder_t decoders[RAILCOM_MAX_CHANNELS];
static rc_channel_t channels[RAILCOM_MAX_CHANNELS];

// Shared core 1 -> core 0 (written by core 1, read+cleared by core 0)
static volatile bool     cutout_flag[RAILCOM_MAX_CHANNELS];
static volatile uint16_t result_addr_nand[RAILCOM_MAX_CHANNELS];
static volatile uint16_t result_addr_or[RAILCOM_MAX_CHANNELS];
static volatile bool     result_valid_nand[RAILCOM_MAX_CHANNELS];
static volatile bool     result_valid_or[RAILCOM_MAX_CHANNELS];

// DMA restart flag (core 0 ISR -> core 1)
static volatile bool dma_restarted;

// Diagnostics (written by core 1, read by core 0)
#define TRACE_MAX_SAMPLES   960   // must be multiple of 8; covers full 900-sample cutout
#define TRACE_BYTES         (TRACE_MAX_SAMPLES / 8)  // 120
#define TRACE_LINE_SAMPLES  80    // samples per printed line (10 bit periods)

static volatile struct {
    uint32_t total_samples;
    uint32_t cutouts_detected[RAILCOM_MAX_CHANNELS];
    // Ch0 A/B pin trace: one cutout's worth of packed pin samples
    uint8_t trace_a[TRACE_BYTES];
    uint8_t trace_b[TRACE_BYTES];
    uint16_t trace_len;        // actual samples captured (up to TRACE_MAX_SAMPLES)
    bool trace_ready;          // set by core 1 at cutout end, cleared by core 0 after print
} rc_diag;

// ---------------------------------------------------------------------------
// Software UART
// ---------------------------------------------------------------------------
static void soft_uart_reset(soft_uart_t *u) {
    u->state = UART_IDLE;
    u->sample_count = 0;
    u->bit_count = 0;
    u->shift_reg = 0;
    u->last_val = true;   // NAND and OR signals are HIGH during cutout idle (A != B)
}

static inline void soft_uart_process(soft_uart_t *u, bool val,
                                     uint8_t *buf, uint8_t *count) {
    switch (u->state) {
    case UART_IDLE:
        if (u->last_val && !val) {
            u->state = UART_START_BIT;
            u->sample_count = 1;
        }
        break;

    case UART_START_BIT:
        u->sample_count++;
        if (u->sample_count == SAMPLES_PER_BIT / 2) {
            if (!val) {
                u->state = UART_DATA_BITS;
                u->sample_count = 0;
                u->bit_count = 0;
                u->shift_reg = 0;
            } else {
                u->state = UART_IDLE;
            }
        }
        break;

    case UART_DATA_BITS:
        u->sample_count++;
        if (u->sample_count == SAMPLES_PER_BIT) {
            u->shift_reg >>= 1;
            if (val) u->shift_reg |= 0x80;
            u->bit_count++;
            u->sample_count = 0;
            if (u->bit_count == 8) {
                u->state = UART_STOP_BIT;
            }
        }
        break;

    case UART_STOP_BIT:
        u->sample_count++;
        if (u->sample_count == SAMPLES_PER_BIT) {
            if (val && *count < RC_MAX_BYTES) {
                buf[(*count)++] = u->shift_reg;
            }
            u->state = UART_IDLE;
        }
        break;
    }

    u->last_val = val;
}

// ---------------------------------------------------------------------------
// RailCom address decode
// ---------------------------------------------------------------------------
static bool decode_address(const uint8_t *buf, uint8_t count, uint16_t *addr) {
    if (count < 2) return false;

    for (int i = 0; i <= count - 2; i++) {
        uint8_t hi = decode_table[buf[i]];
        uint8_t lo = decode_table[buf[i + 1]];
        if (hi < 64 && lo < 64) {
            *addr = ((uint16_t)hi << 6) | lo;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Core 1: sample processing
// ---------------------------------------------------------------------------
static void process_cutout_end(int ch) {
    rc_decoder_t *d = &decoders[ch];
    uint16_t addr;

    bool vn = decode_address(d->rx_buf_nand, d->rx_count_nand, &addr);
    if (vn) result_addr_nand[ch] = addr;

    bool vo = decode_address(d->rx_buf_or, d->rx_count_or, &addr);
    if (vo) result_addr_or[ch] = addr;

    result_valid_nand[ch] = vn;
    result_valid_or[ch] = vo;
    __dmb();
    cutout_flag[ch] = true;
}

static void process_sample(uint8_t sample) {
    rc_diag.total_samples++;

    for (int ch = 0; ch < RAILCOM_MAX_CHANNELS; ch++) {
        rc_decoder_t *d = &decoders[ch];
        bool a = (sample >> (ch * 2)) & 1;
        bool b = (sample >> (ch * 2 + 1)) & 1;

        if (!d->in_cutout) {
            if (a != b) {
                d->silence_count++;
                if (d->silence_count >= SILENCE_THRESHOLD) {
                    d->in_cutout = true;
                    d->cutout_count = 0;
                    soft_uart_reset(&d->uart_nand);
                    soft_uart_reset(&d->uart_or);
                    d->rx_count_nand = 0;
                    d->rx_count_or = 0;
                    rc_diag.cutouts_detected[ch]++;
                }
            } else {
                d->silence_count = 0;
            }
        } else {
            d->cutout_count++;

            // Capture Ch0 A/B pin trace for diagnostics
            if (ch == 0 && !rc_diag.trace_ready) {
                uint16_t pos = d->cutout_count - 1;
                if (pos < TRACE_MAX_SAMPLES) {
                    if (a) rc_diag.trace_a[pos / 8] |= (1u << (pos % 8));
                    if (b) rc_diag.trace_b[pos / 8] |= (1u << (pos % 8));
                }
            }

            soft_uart_process(&d->uart_nand, !(a && b),
                              d->rx_buf_nand, &d->rx_count_nand);
            soft_uart_process(&d->uart_or, (a || b),
                              d->rx_buf_or, &d->rx_count_or);

            if (d->cutout_count >= CUTOUT_WINDOW) {
                if (ch == 0 && !rc_diag.trace_ready) {
                    rc_diag.trace_len = (d->cutout_count < TRACE_MAX_SAMPLES) ?
                                         d->cutout_count : TRACE_MAX_SAMPLES;
                    rc_diag.trace_ready = true;
                }
                process_cutout_end(ch);
                d->in_cutout = false;
                d->silence_count = 0;
            }
        }
    }
}

static void __attribute__((noreturn)) core1_entry(void) {
    uint32_t read_idx = 0;

    for (;;) {
        // Check for DMA restart (happens every ~143 min)
        if (dma_restarted) {
            dma_restarted = false;
            read_idx = 0;
            for (int ch = 0; ch < RAILCOM_MAX_CHANNELS; ch++) {
                decoders[ch].in_cutout = false;
                decoders[ch].silence_count = 0;
            }
        }

        // How far has DMA written?
        uint32_t write_idx =
            ((uintptr_t)dma_hw->ch[dma_chan].write_addr -
             (uintptr_t)ring_buf) / sizeof(uint32_t);
        write_idx &= RING_MASK;

        // Process new words
        while (read_idx != write_idx) {
            uint32_t word = ring_buf[read_idx];
            // With shift_right, little-endian ARM byte order is chronological:
            //   byte[0] = oldest sample, byte[3] = newest
            uint8_t *s = (uint8_t *)&word;
            process_sample(s[0]);
            process_sample(s[1]);
            process_sample(s[2]);
            process_sample(s[3]);
            read_idx = (read_idx + 1) & RING_MASK;
        }

        tight_loop_contents();
    }
}

// ---------------------------------------------------------------------------
// DMA restart ISR (runs on core 0)
// ---------------------------------------------------------------------------
static void dma_irq_handler(void) {
    dma_hw->ints0 = 1u << dma_chan;
    dma_restarted = true;
    __dmb();
    dma_channel_set_write_addr(dma_chan, ring_buf, false);
    dma_channel_set_trans_count(dma_chan, DMA_XFER_COUNT, true);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void railcom_init(void) {
    const uint8_t pins[] = {
        RC_CH1_A, RC_CH1_B, RC_CH2_A, RC_CH2_B,
        RC_CH3_A, RC_CH3_B, RC_CH4_A, RC_CH4_B,
    };

    // PIO setup (use PIO1; PIO0 is reserved for CAN2040)
    sample_pio = pio1;
    uint offset = pio_add_program(sample_pio, &railcom_sample_program);
    sample_sm = pio_claim_unused_sm(sample_pio, true);
    railcom_sample_program_init(sample_pio, sample_sm, offset, RC_CH1_A);

    // Configure GPIO pins AFTER PIO init — ensure input enable and pull-ups are active
    for (int i = 0; i < 8; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
        gpio_set_input_enabled(pins[i], true);
    }

    // DMA setup: PIO RX FIFO -> ring buffer
    dma_chan = dma_claim_unused_channel(true);
    dma_cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_cfg, false);
    channel_config_set_write_increment(&dma_cfg, true);
    channel_config_set_ring(&dma_cfg, true, RING_BITS);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(sample_pio, sample_sm, false));

    dma_channel_configure(dma_chan, &dma_cfg,
        ring_buf,                              // write addr
        &sample_pio->rxf[sample_sm],           // read addr (PIO RX FIFO)
        DMA_XFER_COUNT,                        // transfer count
        true);                                 // start now

    // DMA completion interrupt (for restart after ~143 min)
    dma_channel_set_irq0_enabled(dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Start PIO sampling (DMA is already draining)
    pio_sm_set_enabled(sample_pio, sample_sm, true);

    // Init decoder and channel state
    memset(decoders, 0, sizeof(decoders));
    memset(channels, 0, sizeof(channels));
    for (int i = 0; i < RAILCOM_MAX_CHANNELS; i++) {
        channels[i].last_valid_cutout = get_absolute_time();
        railcom_on_signal(i, false);
    }

    // Launch core 1 for sample processing
    multicore_launch_core1(core1_entry);

    DBG("RailCom: PIO sampler at 2MHz, core 1 decode active\n");
}

void railcom_run(void) {
    absolute_time_t now = get_absolute_time();

    // One-shot confirmation that main loop is running
    static bool first_run = true;
    if (first_run) {
        first_run = false;
        DBG("RailCom: railcom_run() entered - main loop OK\n");
    }

    // Periodic diagnostics every 2 seconds
    static absolute_time_t next_diag;
    if (absolute_time_diff_us(next_diag, now) > 0) {
        next_diag = make_timeout_time_ms(2000);
        // Direct GPIO read bypassing PIO/DMA — shows actual pin state right now
        uint32_t gpio_all = sio_hw->gpio_in;
        uint8_t rc_pins = (gpio_all >> RC_CH1_A) & 0xFF;

        // Raw word from ring buffer near DMA write pointer
        uint32_t wr = ((uintptr_t)dma_hw->ch[dma_chan].write_addr -
                       (uintptr_t)ring_buf) / sizeof(uint32_t);
        wr &= RING_MASK;
        uint32_t raw_word = ring_buf[(wr - 1) & RING_MASK];

        DBG("RC: samples=%u cutouts=%u/%u/%u/%u gpio=0x%02X raw=0x%08X\n",
            rc_diag.total_samples,
            rc_diag.cutouts_detected[0], rc_diag.cutouts_detected[1],
            rc_diag.cutouts_detected[2], rc_diag.cutouts_detected[3],
            rc_pins, raw_word);

        if (rc_diag.trace_ready) {
            // Cutout-gated trace from core 1
            uint16_t len = rc_diag.trace_len;
            DBG("RC Ch0 cutout (%u samp, |=8samp/1bit):\n", len);
            for (uint16_t off = 0; off < len; off += TRACE_LINE_SAMPLES) {
                uint16_t end = off + TRACE_LINE_SAMPLES;
                if (end > len) end = len;
                DBG("  [%3u] A: ", off);
                for (uint16_t t = off; t < end; t++) {
                    DBG("%c", (rc_diag.trace_a[t / 8] & (1u << (t % 8))) ? '1' : '0');
                    if ((t + 1) % 8 == 0 && t + 1 < end) DBG("|");
                }
                DBG("\n");
                DBG("        B: ");
                for (uint16_t t = off; t < end; t++) {
                    DBG("%c", (rc_diag.trace_b[t / 8] & (1u << (t % 8))) ? '1' : '0');
                    if ((t + 1) % 8 == 0 && t + 1 < end) DBG("|");
                }
                DBG("\n");
            }
            memset((void *)rc_diag.trace_a, 0, TRACE_BYTES);
            memset((void *)rc_diag.trace_b, 0, TRACE_BYTES);
            __dmb();
            rc_diag.trace_ready = false;
        } else {
            // No cutout — snapshot DMA ring buffer for raw pin visibility
            uint32_t write_idx =
                ((uintptr_t)dma_hw->ch[dma_chan].write_addr -
                 (uintptr_t)ring_buf) / sizeof(uint32_t);
            write_idx &= RING_MASK;
            uint32_t start_idx = (write_idx - TRACE_MAX_SAMPLES / 4) & RING_MASK;
            uint16_t len = TRACE_MAX_SAMPLES;

            DBG("RC Ch0 raw (%u samp, |=8samp/1bit):\n", len);
            for (uint16_t off = 0; off < len; off += TRACE_LINE_SAMPLES) {
                uint16_t end = off + TRACE_LINE_SAMPLES;
                if (end > len) end = len;
                DBG("  [%3u] A: ", off);
                for (uint16_t t = off; t < end; t++) {
                    uint32_t word = ring_buf[(start_idx + t / 4) & RING_MASK];
                    uint8_t s = ((uint8_t *)&word)[t % 4];
                    DBG("%c", (s & 0x01) ? '1' : '0');
                    if ((t + 1) % 8 == 0 && t + 1 < end) DBG("|");
                }
                DBG("\n");
                DBG("        B: ");
                for (uint16_t t = off; t < end; t++) {
                    uint32_t word = ring_buf[(start_idx + t / 4) & RING_MASK];
                    uint8_t s = ((uint8_t *)&word)[t % 4];
                    DBG("%c", (s & 0x02) ? '1' : '0');
                    if ((t + 1) % 8 == 0 && t + 1 < end) DBG("|");
                }
                DBG("\n");
            }
        }
    }

    for (int i = 0; i < RAILCOM_MAX_CHANNELS; i++) {
        rc_channel_t *ch = &channels[i];

        if (cutout_flag[i]) {
            __dmb();
            cutout_flag[i] = false;

            ch->last_valid_cutout = now;
            if (!ch->signal_active) {
                ch->signal_active = true;
                railcom_on_signal(i, true);
            }

            // Check NAND result (Orientation 1)
            if (result_valid_nand[i]) {
                uint16_t addr = result_addr_nand[i];
                if (addr != ch->last_nand.address || !ch->last_nand.occupied) {
                    ch->last_nand.address = addr;
                    ch->last_nand.occupied = true;
                    ch->last_nand_valid = true;
                    railcom_on_data(i, &ch->last_nand);
                    DBG("RailCom Ch%d/NAND: Addr %d (Pos)\n", i, addr);
                }
            }

            // Check OR result (Orientation 2)
            if (result_valid_or[i]) {
                uint16_t addr = result_addr_or[i];
                if (addr != ch->last_or.address || !ch->last_or.occupied) {
                    ch->last_or.address = addr;
                    ch->last_or.occupied = true;
                    ch->last_nand_valid = false;
                    railcom_on_data(i, &ch->last_or);
                    DBG("RailCom Ch%d/OR: Addr %d (Neg)\n", i, addr);
                }
            }
        }

        // Signal loss: no cutout for >500ms
        if (ch->signal_active &&
            absolute_time_diff_us(ch->last_valid_cutout, now) > 500000) {
            ch->signal_active = false;
            ch->last_nand.occupied = false;
            ch->last_nand.address = 0;
            ch->last_or.occupied = false;
            ch->last_or.address = 0;
            railcom_on_signal(i, false);
        }
    }
}
