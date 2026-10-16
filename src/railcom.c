#include "railcom.h"
#include "board.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "railcom_sample.pio.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include "util/dbg.h"

// ---------------------------------------------------------------------------
// Hardware
//
// Each block has a 1.8 Ω shunt in its TRACK_Rn lead, read by a TLV3202
// window comparator and passed through an ISO7720 to two GPIOs:
//   A: high while current flows from the TRACK_Rn rail into BOOSTER_R
//   B: high while current flows from BOOSTER_R out to the TRACK_Rn rail
// Both are low below ~8 mA. A RailCom "0" bit is at least 10 mA in one
// direction or the other, so the UART line is !(A || B): idle (mark) when
// both are low.
//
// CUTOUT_SIGNAL comes from an AC-input optocoupler across the booster
// output: low while DCC is on the rails, high while the booster holds the
// cutout. Its falling edge follows the end of the cutout within a few µs,
// while its rising edge lags the start by the opto's slow turn-off, so all
// timing is measured back from the falling edge.
//
// A PIO state machine samples 16 GPIOs at 2 MHz into a DMA ring buffer. The
// falling edge of CUTOUT_SIGNAL wakes railcom_task, which finds the cutout
// in the ring, copies it out and decodes all four blocks.
//
// "Block" below means one detector input (the public API calls it a
// channel); "channel 1" and "channel 2" are the RailCom datagram windows.
// ---------------------------------------------------------------------------

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

#define CODE_INVALID        0xFF  // decode_table value for a byte that isn't a 4/8 code

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define SAMPLE_RATE_HZ      2000000
#define SAMPLES_PER_US      (SAMPLE_RATE_HZ / 1000000)  // 2
#define SAMPLES_PER_BIT     (SAMPLE_RATE_HZ / 250000)   // 8 at 250 kbps

// Ring buffer of 16-bit samples, written forever by DMA: 16 KB = 4.1 ms
#define RING_BITS           14
#define RING_BYTES          (1u << RING_BITS)
#define RING_SAMPLES        (RING_BYTES / sizeof(uint16_t))
#define RING_MASK           (RING_SAMPLES - 1)

// Times are in µs after the zero crossing of the packet end bit (S-9.3.2
// Table 1). The CUTOUT_SIGNAL falling edge is taken as the nominal cutout
// end (TCE 454-488 µs) plus ~4 µs of opto turn-on, which puts every time
// estimate within about ±20 µs of the true time.
#define CUTOUT_END_US       475
#define WINDOW_START_US     40    // after the cutout starts (TCS 26-32 µs)
#define WINDOW_END_GAP      4     // samples dropped just before the falling edge
#define CH1_START_MIN_US    55    // channel 1 starts at TTS1 = 80 µs
#define CH2_START_MIN_US    165   // last channel 1 byte starts by 137 µs; channel 2 at TTS2 = 193 µs

// A CUTOUT_SIGNAL high period in this range counts as a cutout. The cutout
// lasts 422-462 µs; the opto's turn-off delay shortens what the pin shows.
#define CUTOUT_MIN_US       200
#define CUTOUT_MAX_US       600

#define WINDOW_OFFSET       ((CUTOUT_END_US - WINDOW_START_US) * SAMPLES_PER_US)
#define WINDOW_SAMPLES      (WINDOW_OFFSET - WINDOW_END_GAP)
#define SEARCH_LIMIT        (RING_SAMPLES / 2)  // how far back to look for the falling edge
#define COPY_DEADLINE_US    1000                // window must be copied out within this

// Each bit is read as the majority of samples 3, 4 and 5 of its 8.
#define BIT_SAMPLE_OFFSET   3
#define BYTE_LAST_SAMPLE    (9 * SAMPLES_PER_BIT + BIT_SAMPLE_OFFSET + 2)  // last sample of the stop bit read
#define MIN_IDLE_SAMPLES    SAMPLES_PER_BIT  // idle needed before a byte that doesn't follow another

#define ADDR_PAIR_MAX_US    250000   // ADR1 and ADR2 must arrive this close together
#define ADDR_TIMEOUT_US     1000000  // no address datagram for this long: block vacated
#define SIGNAL_TIMEOUT_US   500000   // no cutout for this long: RailCom signal lost
#define HOUSEKEEPING_MS     100
#define DIAG_PERIOD_US      2000000

// Set to a block index (0-3) to print that block's A/B pins and
// CUTOUT_SIGNAL for one cutout with each diagnostic report.
#ifndef RAILCOM_TRACE_BLOCK
#define RAILCOM_TRACE_BLOCK (-1)
#endif

// ---------------------------------------------------------------------------
// Pin to sample-bit mapping
// ---------------------------------------------------------------------------
#define RC_BIT(pin)         ((pin) - RC_SAMPLE_BASE_PIN)
#define RC_PIN_OK(pin)      ((pin) >= RC_SAMPLE_BASE_PIN && (pin) < RC_SAMPLE_BASE_PIN + 16)

_Static_assert(RC_PIN_OK(RC_CH1_A) && RC_PIN_OK(RC_CH1_B) &&
               RC_PIN_OK(RC_CH2_A) && RC_PIN_OK(RC_CH2_B) &&
               RC_PIN_OK(RC_CH3_A) && RC_PIN_OK(RC_CH3_B) &&
               RC_PIN_OK(RC_CH4_A) && RC_PIN_OK(RC_CH4_B) &&
               RC_PIN_OK(RC_CUTOUT_PIN),
               "RailCom pins must fall within the 16 GPIOs the PIO samples");

#define CUTOUT_MASK         ((uint16_t)(1u << RC_BIT(RC_CUTOUT_PIN)))

static const uint16_t blk_a_mask[RAILCOM_MAX_CHANNELS] = {
    1u << RC_BIT(RC_CH1_A), 1u << RC_BIT(RC_CH2_A),
    1u << RC_BIT(RC_CH3_A), 1u << RC_BIT(RC_CH4_A),
};
static const uint16_t blk_b_mask[RAILCOM_MAX_CHANNELS] = {
    1u << RC_BIT(RC_CH1_B), 1u << RC_BIT(RC_CH2_B),
    1u << RC_BIT(RC_CH3_B), 1u << RC_BIT(RC_CH4_B),
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
    // Channel 1 address halves: ADR1 (adr_high) and ADR2 (adr_low) arrive
    // in alternate cutouts. A receive time of 0 means not received.
    uint8_t  adr_high;
    uint8_t  adr_low;
    uint64_t high_us;
    uint64_t low_us;
    uint64_t last_adr_us;

    // An assembled address must repeat before it is reported
    railcom_data_t candidate;
    uint8_t candidate_count;

    railcom_data_t reported;
    bool known;  // reported at least once
} rc_block_t;

static uint16_t __attribute__((aligned(RING_BYTES))) ring[RING_SAMPLES];
static uint16_t window[WINDOW_SAMPLES];

static PIO sample_pio;
static uint sample_sm;
static uint dma_chan;
static TaskHandle_t rc_task;

static rc_block_t blocks[RAILCOM_MAX_CHANNELS];
static bool signal_active;
static uint64_t last_cutout_us;

static struct {
    uint32_t edges;          // CUTOUT_SIGNAL falling edges
    uint32_t cutouts;        // accepted cutouts
    uint32_t short_high;     // high period below CUTOUT_MIN_US (glitch, or opto too slow)
    uint32_t long_high;      // high period above CUTOUT_MAX_US (track power returning)
    uint32_t no_edge;        // woken but no falling edge found in the ring
    uint32_t overruns;       // window overwritten before it was copied
    uint32_t high_min_us;    // observed high period, reset at each report
    uint32_t high_max_us;
    uint32_t bytes[RAILCOM_MAX_CHANNELS];      // bytes in the channel 1/2 windows
    uint32_t framing[RAILCOM_MAX_CHANNELS];    // bytes with a bad stop bit
    uint32_t ch1_ok[RAILCOM_MAX_CHANNELS];     // valid channel 1 datagrams
    uint32_t ch1_bad[RAILCOM_MAX_CHANNELS];    // wrong byte count or invalid code (often collisions)
    uint32_t ch2_bytes[RAILCOM_MAX_CHANNELS];
    uint32_t addr_bad[RAILCOM_MAX_CHANNELS];   // ADR1/ADR2 pair that isn't a valid address
} stats = { .high_min_us = UINT32_MAX };

#if RAILCOM_TRACE_BLOCK >= 0
static uint16_t trace[WINDOW_SAMPLES];
static bool trace_ready;
#endif

// ---------------------------------------------------------------------------
// Byte decoding
// ---------------------------------------------------------------------------

// True if the line is in the space state (current flowing, a "0") for most
// of the samples read from the bit that starts at w[i].
static inline bool bit_is_space(const uint16_t *w, int i, uint16_t m)
{
    const uint16_t *s = &w[i + BIT_SAMPLE_OFFSET];
    return ((s[0] & m) != 0) + ((s[1] & m) != 0) + ((s[2] & m) != 0) >= 2;
}

typedef enum { BYTE_OK, BYTE_GLITCH, BYTE_FRAMING } byte_result_t;

// Reads a UART byte (start bit, 8 data bits LSB first, stop bit) whose
// start bit begins at w[i].
static byte_result_t read_byte(const uint16_t *w, int i, uint16_t m, uint8_t *out)
{
    if (!bit_is_space(w, i, m))
        return BYTE_GLITCH;

    uint8_t v = 0;
    for (int b = 0; b < 8; b++) {
        if (!bit_is_space(w, i + (b + 1) * SAMPLES_PER_BIT, m))
            v |= 1u << b;
    }

    if (bit_is_space(w, i + 9 * SAMPLES_PER_BIT, m))
        return BYTE_FRAMING;

    *out = v;
    return BYTE_OK;
}

// ---------------------------------------------------------------------------
// Address handling (S-9.3.2 Table 19)
// ---------------------------------------------------------------------------
static bool assemble_address(uint8_t hi, uint8_t lo, railcom_data_t *d)
{
    if (hi == 0x00) {                 // base address: 0 A6..A0
        if (lo & 0x80)
            return false;
        d->address = lo;
        d->addr_type = RAILCOM_ADDR_SHORT;
    } else if (hi == 0x60) {          // consist address: R A6..A0 (R = direction)
        d->address = lo & 0x7F;
        d->addr_type = RAILCOM_ADDR_CONSIST;
    } else if ((hi & 0xC0) == 0x80) { // extended address: 10 A13..A8, A7..A0
        d->address = ((uint16_t)(hi & 0x3F) << 8) | lo;
        d->addr_type = RAILCOM_ADDR_LONG;
    } else {
        return false;
    }
    return d->address != 0;
}

static bool same_data(const railcom_data_t *a, const railcom_data_t *b)
{
    return a->address == b->address && a->occupied == b->occupied &&
           a->addr_type == b->addr_type && a->orientation == b->orientation;
}

static void handle_ch1(int blk, uint8_t id, uint8_t data,
                       railcom_orient_t orient, uint64_t now)
{
    rc_block_t *b = &blocks[blk];
    uint64_t other_us;

    if (id == 1) {
        b->adr_high = data;
        b->high_us = now;
        other_us = b->low_us;
    } else if (id == 2) {
        b->adr_low = data;
        b->low_us = now;
        other_us = b->high_us;
    } else {
        return;  // ID 3 (driving info) and others aren't used
    }

    b->last_adr_us = now;
    if (other_us == 0 || now - other_us > ADDR_PAIR_MAX_US)
        return;

    railcom_data_t d = { .occupied = true, .orientation = orient };
    if (!assemble_address(b->adr_high, b->adr_low, &d)) {
        stats.addr_bad[blk]++;
        return;
    }

    if (same_data(&d, &b->candidate)) {
        if (b->candidate_count < UINT8_MAX)
            b->candidate_count++;
    } else {
        b->candidate = d;
        b->candidate_count = 1;
    }

    if (b->candidate_count >= 2 && !same_data(&b->candidate, &b->reported)) {
        b->reported = b->candidate;
        b->known = true;
        railcom_on_data(blk, &b->reported);
    }
}

// ---------------------------------------------------------------------------
// Cutout decoding
// ---------------------------------------------------------------------------
static void decode_block(int blk, const uint16_t *w, uint64_t now)
{
    const uint16_t am = blk_a_mask[blk];
    const uint16_t bm = blk_b_mask[blk];
    const uint16_t m = am | bm;

    uint8_t ch1[2];
    int n1 = 0;
    bool ch1_overflow = false;
    uint32_t a_votes = 0, b_votes = 0;  // channel 1 current direction

    int idle = 0;
    bool armed = false;

    for (int i = 0; i + BYTE_LAST_SAMPLE < WINDOW_SAMPLES; ) {
        if (!(w[i] & m)) {
            if (++idle >= MIN_IDLE_SAMPLES)
                armed = true;
            i++;
            continue;
        }
        if (!armed) {
            // Current with no idle before it, e.g. DCC load current still
            // flowing at the start of the window.
            idle = 0;
            i++;
            continue;
        }

        uint8_t raw;
        byte_result_t r = read_byte(w, i, m, &raw);
        if (r != BYTE_OK) {
            if (r == BYTE_FRAMING)
                stats.framing[blk]++;
            armed = false;
            idle = 0;
            i++;
            continue;
        }

        int t_us = WINDOW_START_US + i / SAMPLES_PER_US;
        if (t_us >= CH2_START_MIN_US) {
            stats.bytes[blk]++;
            stats.ch2_bytes[blk]++;
        } else if (t_us >= CH1_START_MIN_US) {
            stats.bytes[blk]++;
            if (n1 < 2)
                ch1[n1++] = raw;
            else
                ch1_overflow = true;
            for (int k = i; k < i + 9 * SAMPLES_PER_BIT; k++) {
                if (w[k] & am) a_votes++;
                if (w[k] & bm) b_votes++;
            }
        }

        // The next start bit may follow the stop bit directly
        i += BYTE_LAST_SAMPLE + 1;
        idle = 0;
    }

    if (n1 == 0)
        return;
    if (n1 != 2 || ch1_overflow) {
        stats.ch1_bad[blk]++;
        return;
    }

    uint8_t v0 = decode_table[ch1[0]];
    uint8_t v1 = decode_table[ch1[1]];
    if (v0 >= 0x40 || v1 >= 0x40) {
        stats.ch1_bad[blk]++;
        return;
    }
    stats.ch1_ok[blk]++;

    // Channel 1 carries one 12-bit datagram: 4-bit ID, 8 data bits
    railcom_orient_t orient = a_votes > b_votes ? RAILCOM_ORIENT_A :
                              b_votes > a_votes ? RAILCOM_ORIENT_B :
                                                  RAILCOM_ORIENT_UNKNOWN;
    handle_ch1(blk, v0 >> 2, (uint8_t)(((v0 & 0x03) << 6) | v1), orient, now);
}

static uint32_t dma_write_index(void)
{
    uintptr_t wa = (uintptr_t)dma_hw->ch[dma_chan].write_addr;
    return ((wa - (uintptr_t)ring) / sizeof(uint16_t)) & RING_MASK;
}

static inline bool cutout_at(uint32_t idx)
{
    return (ring[idx & RING_MASK] & CUTOUT_MASK) != 0;
}

static void process_cutout(void)
{
    uint32_t t0 = time_us_32();
    uint32_t newest = dma_write_index() - 1;

    // Find the most recent CUTOUT_SIGNAL falling edge
    uint32_t fall = 0;
    bool found = false;
    for (uint32_t n = 0; n < SEARCH_LIMIT; n++) {
        uint32_t k = newest - n;
        if (!cutout_at(k) && cutout_at(k - 1)) {
            fall = k;
            found = true;
            break;
        }
    }
    if (!found) {
        stats.no_edge++;
        return;
    }

    // Measure the high period that ends there
    uint32_t high = 0;
    while (high <= CUTOUT_MAX_US * SAMPLES_PER_US && cutout_at(fall - 1 - high))
        high++;

    uint32_t high_us = high / SAMPLES_PER_US;
    if (high_us < stats.high_min_us) stats.high_min_us = high_us;
    if (high_us > stats.high_max_us) stats.high_max_us = high_us;

    if (high < CUTOUT_MIN_US * SAMPLES_PER_US) {
        stats.short_high++;
        return;
    }
    if (high > CUTOUT_MAX_US * SAMPLES_PER_US) {
        stats.long_high++;
        return;
    }

    // Copy the window out before DMA comes back around to it
    uint32_t start = fall - WINDOW_OFFSET;
    for (uint32_t i = 0; i < WINDOW_SAMPLES; i++)
        window[i] = ring[(start + i) & RING_MASK];
    if (time_us_32() - t0 > COPY_DEADLINE_US) {
        stats.overruns++;
        return;
    }

    stats.cutouts++;
    uint64_t now = time_us_64();
    last_cutout_us = now;

#if RAILCOM_TRACE_BLOCK >= 0
    uint32_t traced_bytes = stats.bytes[RAILCOM_TRACE_BLOCK];
#endif

    for (int blk = 0; blk < RAILCOM_MAX_CHANNELS; blk++)
        decode_block(blk, window, now);

#if RAILCOM_TRACE_BLOCK >= 0
    // Keep the latest cutout with data on the traced block, or any cutout
    // until one has data
    if (!trace_ready || stats.bytes[RAILCOM_TRACE_BLOCK] != traced_bytes) {
        memcpy(trace, window, sizeof(trace));
        trace_ready = true;
    }
#endif
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
#if RAILCOM_TRACE_BLOCK >= 0
static void print_trace(void)
{
    static const struct { char name; uint16_t mask; } rows[] = {
        { 'A', blk_a_mask[RAILCOM_TRACE_BLOCK] },
        { 'B', blk_b_mask[RAILCOM_TRACE_BLOCK] },
        { 'C', CUTOUT_MASK },
    };
    const int us_per_line = 100;
    char line[us_per_line + 1];

    DBG("RC T%d trace: 1 char/us, '1' = high, C = CUTOUT_SIGNAL\n", RAILCOM_TRACE_BLOCK + 1);
    for (int off = 0; off < WINDOW_SAMPLES; off += us_per_line * SAMPLES_PER_US) {
        for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
            int len = 0;
            for (int k = off; k + 1 < WINDOW_SAMPLES && len < us_per_line; k += SAMPLES_PER_US)
                line[len++] = ((trace[k] | trace[k + 1]) & rows[r].mask) ? '1' : '.';
            line[len] = '\0';
            DBG("  %3dus %c %s\n", WINDOW_START_US + off / SAMPLES_PER_US, rows[r].name, line);
        }
    }
    trace_ready = false;
}
#endif

static void print_diag(void)
{
    static const char *const type_name[] = { "short", "long", "consist" };
    static const char *const orient_name[] = { "?", "A", "B" };

    DBG("RC: edges=%lu cutouts=%lu short=%lu long=%lu noedge=%lu overrun=%lu",
        (unsigned long)stats.edges, (unsigned long)stats.cutouts,
        (unsigned long)stats.short_high, (unsigned long)stats.long_high,
        (unsigned long)stats.no_edge, (unsigned long)stats.overruns);
    if (stats.high_max_us)
        DBG(" high=%lu-%luus", (unsigned long)stats.high_min_us, (unsigned long)stats.high_max_us);
    DBG("\n");
    stats.high_min_us = UINT32_MAX;
    stats.high_max_us = 0;

    for (int blk = 0; blk < RAILCOM_MAX_CHANNELS; blk++) {
        const railcom_data_t *d = &blocks[blk].reported;
        DBG("RC T%d: bytes=%lu ferr=%lu ch1=%lu/%lu bad ch2=%lu adrbad=%lu",
            blk + 1, (unsigned long)stats.bytes[blk], (unsigned long)stats.framing[blk],
            (unsigned long)stats.ch1_ok[blk], (unsigned long)stats.ch1_bad[blk],
            (unsigned long)stats.ch2_bytes[blk], (unsigned long)stats.addr_bad[blk]);
        if (d->occupied)
            DBG(" loco=%u (%s, %s)", d->address, type_name[d->addr_type],
                orient_name[d->orientation]);
        DBG("\n");
    }

#if RAILCOM_TRACE_BLOCK >= 0
    if (trace_ready)
        print_trace();
#endif
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
static void housekeeping(void)
{
    uint64_t now = time_us_64();

    bool signal = last_cutout_us != 0 && now - last_cutout_us < SIGNAL_TIMEOUT_US;
    if (signal != signal_active) {
        signal_active = signal;
        DBG("RailCom: cutouts %s\n", signal ? "detected" : "lost");
        for (int blk = 0; blk < RAILCOM_MAX_CHANNELS; blk++) {
            // Without cutouts nothing can be detected, so blocks keep their
            // last report; when cutouts resume, a loco gets the usual time
            // to be seen again before its block is reported vacated.
            if (signal)
                blocks[blk].last_adr_us = now;
            railcom_on_signal(blk, signal);
        }
    }

    // A block with no loco seen for ADDR_TIMEOUT_US while cutouts arrive is
    // vacant: report it if it was occupied, or if it hasn't been reported yet
    for (int blk = 0; blk < RAILCOM_MAX_CHANNELS; blk++) {
        rc_block_t *b = &blocks[blk];
        if (signal && now - b->last_adr_us > ADDR_TIMEOUT_US &&
            (b->reported.occupied || !b->known)) {
            b->known = true;
            b->reported.occupied = false;
            b->candidate_count = 0;
            b->high_us = 0;
            b->low_us = 0;
            railcom_on_data(blk, &b->reported);
        }
    }

    static uint64_t next_diag;
    if (now >= next_diag) {
        next_diag = now + DIAG_PERIOD_US;
        print_diag();
    }
}

static void cutout_irq_handler(void)
{
    if (!(gpio_get_irq_event_mask(RC_CUTOUT_PIN) & GPIO_IRQ_EDGE_FALL))
        return;
    gpio_acknowledge_irq(RC_CUTOUT_PIN, GPIO_IRQ_EDGE_FALL);

    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(rc_task, &woken);
    portYIELD_FROM_ISR(woken);
}

void railcom_task(void *param)
{
    (void)param;

    rc_task = xTaskGetCurrentTaskHandle();
    gpio_add_raw_irq_handler(RC_CUTOUT_PIN, cutout_irq_handler);
    gpio_set_irq_enabled(RC_CUTOUT_PIN, GPIO_IRQ_EDGE_FALL, true);
    irq_set_enabled(IO_IRQ_BANK0, true);

    for (;;) {
        uint32_t edges = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HOUSEKEEPING_MS));
        if (edges) {
            stats.edges += edges;
            process_cutout();
        }
        housekeeping();
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void railcom_init(void)
{
    static const uint8_t pins[] = {
        RC_CH1_A, RC_CH1_B, RC_CH2_A, RC_CH2_B,
        RC_CH3_A, RC_CH3_B, RC_CH4_A, RC_CH4_B,
        RC_CUTOUT_PIN,
    };

    // ISO7720 outputs are push-pull and CUTOUT_SIGNAL has its own pull-up (R36)
    for (size_t i = 0; i < sizeof(pins); i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_disable_pulls(pins[i]);
    }

    // PIO0 (PIO1 belongs to can2040)
    sample_pio = pio0;
    uint offset = pio_add_program(sample_pio, &railcom_sample_program);
    sample_sm = pio_claim_unused_sm(sample_pio, true);
    railcom_sample_program_init(sample_pio, sample_sm, offset, RC_SAMPLE_BASE_PIN);

    // DMA: PIO RX FIFO -> ring buffer, endlessly
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_ring(&cfg, true, RING_BITS);
    channel_config_set_dreq(&cfg, pio_get_dreq(sample_pio, sample_sm, false));
    dma_channel_configure(dma_chan, &cfg, ring, &sample_pio->rxf[sample_sm],
                          dma_encode_endless_transfer_count(), true);

    pio_sm_set_enabled(sample_pio, sample_sm, true);

    memset(blocks, 0, sizeof(blocks));
    for (int blk = 0; blk < RAILCOM_MAX_CHANNELS; blk++)
        railcom_on_signal(blk, false);

    DBG("RailCom: sampling GPIO %d-%d at 2 MHz, cutout on GPIO %d\n",
        RC_SAMPLE_BASE_PIN, RC_SAMPLE_BASE_PIN + 15, RC_CUTOUT_PIN);
}
