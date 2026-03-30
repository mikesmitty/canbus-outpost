#ifndef SERVO_H
#define SERVO_H

#include <stdint.h>

#define SERVO_COUNT 4

void servo_init(void);
void servo_set_position(int index, uint16_t value); // 0-65535, where ~32768 is center

#endif // SERVO_H
