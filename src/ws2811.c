#include "ws2811.h"
#include "FreeRTOS.h"
#include "task.h"
#include "board.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "util/dbg.h"
#include "ws2811.pio.h"

// ---------------------------------------------------------------------------
// WS2811 chain for signal heads, on the SSSB header (J15). Each chip keeps the
// first 24 bits it sees, passes the rest along, and all of them apply their
// new values once the line has rested low.
//
// DMA feeds a frame straight from pixels to the PIO, so a frame costs no CPU
// time. Each chip is one word, so a write during a frame can't split a chip's
// values; anything it misses goes out on the next frame.
// ---------------------------------------------------------------------------

// Low time that latches the chips: the datasheet asks for 50 µs, but some
// compatible chips need closer to 280 µs
#define LATCH_US    300

// Time from starting a frame to being able to start the next. vTaskDelay can
// end up to a tick early.
#define FRAME_MS    4

_Static_assert(WS2811_CHIPS * 24 * 1000000ull / WS2811_400KHZ + LATCH_US <= (FRAME_MS - 1) * 1000,
               "a frame and its latch must fit in FRAME_MS");

// Frames also go out this often with nothing new, so a head plugged in later
// or back from a tripped fuse picks up its values
#define REFRESH_MS  1000

static uint32_t pixels[WS2811_CHIPS];
static TaskHandle_t task;
static uint sm;
static uint dma_chan;

void ws2811_init(void)
{
    // PIO2 (PIO0 is RailCom's, PIO1 belongs to can2040)
    uint offset = pio_add_program(pio2, &ws2811_program);
    sm = pio_claim_unused_sm(pio2, true);
    ws2811_program_init(pio2, sm, offset, LIGHTS_PIN, WS2811_800KHZ);

    // DMA: pixels -> PIO TX FIFO, one frame each time it's started
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio2, sm, true));
    dma_channel_configure(dma_chan, &cfg, &pio2->txf[sm], pixels, WS2811_CHIPS, false);

    pio_sm_set_enabled(pio2, sm, true);

    DBG("WS2811: %d chips on GPIO %d\n", WS2811_CHIPS, LIGHTS_PIN);
}

void ws2811_set_bit_rate(uint32_t bit_hz)
{
    pio_sm_set_clkdiv(pio2, sm, ws2811_clkdiv(bit_hz));
}

void ws2811_set(int chip, uint8_t r, uint8_t g, uint8_t b)
{
    if (chip < 0 || chip >= WS2811_CHIPS) return;
    pixels[chip] = (uint32_t)r << 24 | (uint32_t)g << 16 | (uint32_t)b << 8;
}

void ws2811_show(void)
{
    if (task)
        xTaskNotifyGive(task);
}

void ws2811_task(void *params)
{
    (void)params;
    task = xTaskGetCurrentTaskHandle();

    for (;;) {
        dma_channel_set_read_addr(dma_chan, pixels, true);
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(REFRESH_MS));
    }
}
