// Stand-in for the SDK/FreeRTOS header so railcom.c builds on the host
#pragma once
#define IO_IRQ_BANK0 13
static inline void irq_set_enabled(int n, bool e) { (void)n; (void)e; }
