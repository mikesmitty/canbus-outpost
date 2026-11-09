// Stand-in for the FreeRTOS header so the app modules build on the host.
// Timers are recorded by name: a test finds one with mock_timer(), sees
// whether it's running and fires it by calling its callback. A test that
// links code using timers defines mock_timers and mock_ntimers.
#pragma once
#include <stdbool.h>
#include <string.h>
#include "FreeRTOS.h"

typedef struct mock_timer *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);

struct mock_timer {
    const char *name;
    uint32_t period;
    TimerCallbackFunction_t cb;
    bool active;
};

#define MOCK_TIMERS 8
extern struct mock_timer mock_timers[MOCK_TIMERS];
extern int mock_ntimers;

static inline TimerHandle_t mock_timer(const char *name)
{
    for (int i = 0; i < mock_ntimers; i++)
        if (strcmp(mock_timers[i].name, name) == 0)
            return &mock_timers[i];
    return NULL;
}

// Creating a timer again by the same name (a module set up again for the
// next test) replaces it
static inline TimerHandle_t xTimerCreate(const char *n, uint32_t p, BaseType_t r, void *id, TimerCallbackFunction_t cb)
{
    (void)r; (void)id;
    TimerHandle_t t = mock_timer(n);
    if (!t)
        t = &mock_timers[mock_ntimers++];
    *t = (struct mock_timer){ .name = n, .period = p, .cb = cb };
    return t;
}
static inline BaseType_t xTimerStart(TimerHandle_t t, uint32_t w) { (void)w; t->active = true; return pdTRUE; }
static inline BaseType_t xTimerStop(TimerHandle_t t, uint32_t w) { (void)w; t->active = false; return pdTRUE; }
static inline BaseType_t xTimerIsTimerActive(TimerHandle_t t) { return t->active; }
