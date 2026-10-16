// Stand-in for the SDK/FreeRTOS header so railcom.c builds on the host
#pragma once
#include "FreeRTOS.h"
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return 0; }
static inline void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *w) { (void)t; (void)w; }
static inline uint32_t ulTaskNotifyTake(BaseType_t c, uint32_t t) { (void)c; (void)t; return 0; }
