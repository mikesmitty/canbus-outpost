// Stand-in for the SDK/FreeRTOS header so railcom.c builds on the host
#pragma once
#include "pico/stdlib.h"
typedef struct { uint32_t rxf[4]; } pio_hw_t;
typedef pio_hw_t *PIO;
extern pio_hw_t mock_pio0;
#define pio0 (&mock_pio0)
static inline uint pio_claim_unused_sm(PIO p, bool r) { (void)p; (void)r; return 0; }
static inline uint pio_get_dreq(PIO p, uint sm, bool tx) { (void)p; (void)sm; (void)tx; return 0; }
static inline void pio_sm_set_enabled(PIO p, uint sm, bool e) { (void)p; (void)sm; (void)e; }
