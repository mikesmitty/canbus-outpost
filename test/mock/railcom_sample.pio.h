// Stand-in for the SDK/FreeRTOS header so railcom.c builds on the host
#pragma once
#include "hardware/pio.h"
static const int railcom_sample_program = 0;
static inline uint pio_add_program(PIO p, const int *prog) { (void)p; (void)prog; return 0; }
static inline void railcom_sample_program_init(PIO p, uint sm, uint off, uint base) { (void)p; (void)sm; (void)off; (void)base; }
