#include "railcom.h"
#include "railcom_uart.pio.h"
#include "board.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>
#include "util/dbg.h"

// RailCom 4/8 decoding table based on RCN-217.
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

typedef struct {
    uint8_t pin;
    PIO pio;
    uint sm;
    uint8_t buffer[10];
    uint8_t buf_idx;
    absolute_time_t last_edge;
    bool last_pin_state;
    bool in_potential_gap;
} railcom_receiver_t;

typedef struct {
    railcom_receiver_t rx_a;
    railcom_receiver_t rx_b;
    railcom_data_t last_data;
    bool signal_active;
    absolute_time_t last_valid_gap;
} railcom_channel_state_t;

static railcom_channel_state_t channels[RAILCOM_MAX_CHANNELS];

static void rx_init(railcom_receiver_t *rx, PIO pio, uint8_t pin, uint offset) {
    rx->pin = pin;
    rx->pio = pio;
    rx->sm = pio_claim_unused_sm(pio, true);
    rx->buf_idx = 0;
    rx->last_edge = get_absolute_time();
    rx->last_pin_state = false;
    rx->in_potential_gap = false;

    pio_sm_config c = railcom_uart_program_get_default_config(offset);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_in_shift(&c, true, true, 8); 
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    
    float div = (float)clock_get_hz(clk_sys) / (250000.0f * 10.0f);
    sm_config_set_clkdiv(&c, div);
    
    pio_sm_init(pio, rx->sm, offset, &c);
    pio_sm_set_enabled(pio, rx->sm, true);
    
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

void railcom_init(void) {
    uint offset0 = pio_add_program(pio0, &railcom_uart_program);
    uint offset1 = pio_add_program(pio1, &railcom_uart_program);
    uint offset2 = pio_add_program(pio2, &railcom_uart_program);
    
    uint8_t pins_a[] = {RC_CH1_A, RC_CH2_A, RC_CH3_A, RC_CH4_A};
    uint8_t pins_b[] = {RC_CH1_B, RC_CH2_B, RC_CH3_B, RC_CH4_B};
    
    for (int i = 0; i < RAILCOM_MAX_CHANNELS; i++) {
        PIO pio = (i == 0) ? pio0 : (i < 3 ? pio1 : pio2);
        uint offset = (i == 0) ? offset0 : (i < 3 ? offset1 : offset2);
        
        rx_init(&channels[i].rx_a, pio, pins_a[i], offset);
        rx_init(&channels[i].rx_b, pio, pins_b[i], offset);
        
        channels[i].signal_active = false;
        channels[i].last_valid_gap = get_absolute_time();
        railcom_on_signal(i, false);
    }
    DBG("RailCom: Proactive Gap Detection Initialized.\n");
}

static void decode_buffer(int ch, railcom_receiver_t *rx, bool is_b) {
    if (rx->buf_idx < 2) return; 

    uint8_t high_raw = decode_table[rx->buffer[0]];
    uint8_t low_raw = decode_table[rx->buffer[1]];

    if (high_raw < 64 && low_raw < 64) {
        uint16_t address = (uint16_t)high_raw << 6 | (uint16_t)low_raw;
        if (address != channels[ch].last_data.address || !channels[ch].last_data.occupied) {
            channels[ch].last_data.address = address;
            channels[ch].last_data.occupied = true;
            railcom_on_data(ch, &channels[ch].last_data);
            
            DBG("RailCom Ch %d: Addr %d (Pin %s)\n", ch, address, is_b ? "B" : "A");
        }
    }
}

static void process_receiver(int ch, railcom_receiver_t *rx, bool is_b) {
    bool current_pin = gpio_get(rx->pin);
    absolute_time_t now = get_absolute_time();

    // 1. Detect if we are entering a gap (no transitions for > 150us)
    int64_t time_since_edge = absolute_time_diff_us(rx->last_edge, now);
    if (!rx->in_potential_gap && time_since_edge > 150) {
        // Clear DCC garbage from FIFO
        pio_sm_clear_fifos(rx->pio, rx->sm);
        rx->buf_idx = 0;
        rx->in_potential_gap = true;
    }

    // 2. Track transitions to identify gap endings
    if (current_pin != rx->last_pin_state) {
        int64_t dt = absolute_time_diff_us(rx->last_edge, now);
        rx->last_edge = now;
        rx->last_pin_state = current_pin;
        rx->in_potential_gap = false;

        // Valid RailCom gap is ~488us
        if (dt > 400 && dt < 600) {
            // Collect any bytes that arrived during this specific gap
            while (!pio_sm_is_rx_fifo_empty(rx->pio, rx->sm)) {
                uint32_t val = pio_sm_get(rx->pio, rx->sm);
                if (rx->buf_idx < sizeof(rx->buffer)) {
                    rx->buffer[rx->buf_idx++] = (uint8_t)(val >> 24);
                }
            }
            decode_buffer(ch, rx, is_b);
            
            channels[ch].last_valid_gap = now;
            if (!channels[ch].signal_active) {
                channels[ch].signal_active = true;
                railcom_on_signal(ch, true);
            }
        } else if (dt > 300 && dt < 1000) {
            // Optional: Log "near miss" gaps for tuning
        }
    }
}

void railcom_run(void) {
    absolute_time_t now = get_absolute_time();
    for (int i = 0; i < RAILCOM_MAX_CHANNELS; i++) {
        process_receiver(i, &channels[i].rx_a, false);
        process_receiver(i, &channels[i].rx_b, true);

        // Check for signal loss (increased to 500ms for stability)
        if (channels[i].signal_active && absolute_time_diff_us(channels[i].last_valid_gap, now) > 500000) {
            channels[i].signal_active = false;
            railcom_on_signal(i, false);
        }
    }
}
