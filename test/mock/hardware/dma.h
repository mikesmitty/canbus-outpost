// Stand-in for the SDK/FreeRTOS header so railcom.c builds on the host
#pragma once
#include "pico/stdlib.h"
typedef struct { uintptr_t write_addr; } mock_dma_ch_t;
typedef struct { mock_dma_ch_t ch[16]; } mock_dma_hw_t;
extern mock_dma_hw_t mock_dma;
#define dma_hw (&mock_dma)
typedef struct { int x; } dma_channel_config;
#define DMA_SIZE_32 2
static inline uint dma_claim_unused_channel(bool r) { (void)r; return 0; }
static inline dma_channel_config dma_channel_get_default_config(uint c) { (void)c; dma_channel_config x = {0}; return x; }
static inline void channel_config_set_transfer_data_size(dma_channel_config *c, int s) { (void)c; (void)s; }
static inline void channel_config_set_read_increment(dma_channel_config *c, bool b) { (void)c; (void)b; }
static inline void channel_config_set_write_increment(dma_channel_config *c, bool b) { (void)c; (void)b; }
static inline void channel_config_set_ring(dma_channel_config *c, bool w, uint b) { (void)c; (void)w; (void)b; }
static inline void channel_config_set_dreq(dma_channel_config *c, uint d) { (void)c; (void)d; }
static inline uint32_t dma_encode_endless_transfer_count(void) { return 0xffffffffu; }
static inline void dma_channel_configure(uint ch, const dma_channel_config *c, volatile void *w, const volatile void *r, uint n, bool t)
{ (void)c; (void)r; (void)n; (void)t; mock_dma.ch[ch].write_addr = (uintptr_t)w; }
