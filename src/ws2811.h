#ifndef WS2811_H
#define WS2811_H

#include <stdint.h>

// Chips on the chain, one per signal head. Every frame is this long: on a
// shorter chain the extra data just runs off the end.
#define WS2811_CHIPS 16

// Bit rates, set by the chips' SET pin: open = 800 kHz, tied to VDD = 400 kHz.
// A chain must be all one or the other.
#define WS2811_800KHZ 800000
#define WS2811_400KHZ 400000

void ws2811_init(void);
void ws2811_task(void *params);

void ws2811_set_bit_rate(uint32_t bit_hz);

// Where the chain plugs in: 0 is the LED header, 1-4 a servo output
// for boards made before the header. The chain moves before its next frame;
// the pin it leaves rests low until something else takes it.
void ws2811_set_connector(int connector);

// chip counts out from the board. r, g and y are the chip's OUTR, OUTG and
// OUTB duty, 0 (off) to 255 (full): red, green and yellow as SSSB heads are
// wired, though any lamp can be on any output. Values go out on the next
// frame; ws2811_show() asks for one now.
void ws2811_set(int chip, uint8_t r, uint8_t g, uint8_t y);
void ws2811_show(void);

#endif // WS2811_H
