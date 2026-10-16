#ifndef SERVO_H
#define SERVO_H

#include <stdint.h>
#include <stdbool.h>

#define SERVO_COUNT 4
#define SERVO_TICK_MS 20  // servo_tick() period

void servo_init(void);
void servo_set_position(int index, uint16_t value); // 0-65535, instant move
void servo_set_pulse_us(int index, uint16_t us);    // direct pulse width in microseconds
void servo_disable(int index);                       // 0% duty — no signal, no power draw

/* Smooth movement API.
 * throw_time: duration in tenths of seconds (0=instant, 1-255 = 0.1-25.5s)
 * hold_time: seconds to hold position after arrival before cutting PWM (0=hold forever)
 * frog_pin: output channel 1-4 to toggle at midpoint, or 0 for none
 * midpoint: position at which to toggle the frog_pin */
void servo_set_target(int index, uint16_t target, uint8_t throw_time, uint8_t hold_time, uint8_t frog_pin, uint16_t midpoint);

uint8_t servo_tick(void);  // returns bitmask of servos that just completed movement
bool servo_is_moving(int index);
bool servo_busy(void);     // any servo moving or counting down its hold time

#endif // SERVO_H
