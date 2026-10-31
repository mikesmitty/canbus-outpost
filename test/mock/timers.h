// Stand-in for the SDK/FreeRTOS header so signals.c builds on the host
#pragma once
#include <stdbool.h>
#include "FreeRTOS.h"
typedef void *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);
extern TimerCallbackFunction_t mock_timer_cb;
extern bool mock_timer_active;
static inline TimerHandle_t xTimerCreate(const char *n, uint32_t p, BaseType_t r, void *id, TimerCallbackFunction_t cb)
{ (void)n; (void)p; (void)r; (void)id; mock_timer_cb = cb; return (TimerHandle_t)&mock_timer_cb; }
static inline BaseType_t xTimerStart(TimerHandle_t t, uint32_t w) { (void)t; (void)w; mock_timer_active = true; return pdTRUE; }
static inline BaseType_t xTimerStop(TimerHandle_t t, uint32_t w) { (void)t; (void)w; mock_timer_active = false; return pdTRUE; }
static inline BaseType_t xTimerIsTimerActive(TimerHandle_t t) { (void)t; return mock_timer_active; }
