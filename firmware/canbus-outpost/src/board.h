#ifndef BOARD_H
#define BOARD_H

#include "pico/stdlib.h"

// Debug UART0
#define DEBUG_UART uart0
#define DEBUG_TX_PIN 0
#define DEBUG_RX_PIN 1

// CAN Bus (PIO)
#define CAN_TX_PIN 2
#define CAN_RX_PIN 3

// General Purpose I/O
#define GPIO_OUT1 4
#define GPIO_OUT2 5
#define GPIO_OUT3 6
#define GPIO_OUT4 7

// Servo PWM Outputs
#define SERVO1_PIN 12
#define SERVO2_PIN 13
#define SERVO3_PIN 14
#define SERVO4_PIN 15

// RailCom Detector Channels (A and B for Data/Orientation)
#define RC_CH1_A 17
#define RC_CH1_B 18
#define RC_CH2_A 19
#define RC_CH2_B 20
#define RC_CH3_A 21
#define RC_CH3_B 22
#define RC_CH4_A 23
#define RC_CH4_B 24

// Onboard LED (Pico 2/RP2350 usually has a default LED)
#ifndef PICO_DEFAULT_LED_PIN
#define HEARTBEAT_LED 25
#else
#define HEARTBEAT_LED PICO_DEFAULT_LED_PIN
#endif

#endif // BOARD_H
