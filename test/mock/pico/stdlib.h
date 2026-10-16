// Stand-in for the SDK/FreeRTOS header so railcom.c builds on the host
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef unsigned int uint;
extern uint64_t mock_now_us;
static inline uint32_t time_us_32(void) { return (uint32_t)mock_now_us; }
static inline uint64_t time_us_64(void) { return mock_now_us; }
