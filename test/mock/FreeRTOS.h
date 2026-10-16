// Stand-in for the SDK/FreeRTOS header so railcom.c builds on the host
#pragma once
#include <stdint.h>
typedef long BaseType_t;
typedef void *TaskHandle_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portYIELD_FROM_ISR(x) (void)(x)
