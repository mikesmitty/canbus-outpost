// Stand-in for the SDK/FreeRTOS header so railcom.c builds on the host
#pragma once
#include "pico/stdlib.h"
#define GPIO_IN 0
#define GPIO_IRQ_EDGE_FALL 4u
typedef void (*irq_handler_t)(void);
static inline void gpio_init(uint p) { (void)p; }
static inline void gpio_set_dir(uint p, int d) { (void)p; (void)d; }
static inline void gpio_disable_pulls(uint p) { (void)p; }
static inline uint32_t gpio_get_irq_event_mask(uint p) { (void)p; return GPIO_IRQ_EDGE_FALL; }
static inline void gpio_acknowledge_irq(uint p, uint32_t m) { (void)p; (void)m; }
static inline void gpio_add_raw_irq_handler(uint p, irq_handler_t h) { (void)p; (void)h; }
static inline void gpio_set_irq_enabled(uint p, uint32_t e, bool en) { (void)p; (void)e; (void)en; }
