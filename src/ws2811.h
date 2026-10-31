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

// chip counts out from the board. r, g and b are the chip's OUTR, OUTG and
// OUTB duty, 0 (off) to 255 (full), whatever color is wired to each. Values
// go out on the next frame; ws2811_show() asks for one now.
void ws2811_set(int chip, uint8_t r, uint8_t g, uint8_t b);
void ws2811_show(void);

#endif // WS2811_H
