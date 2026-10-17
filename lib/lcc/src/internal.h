#ifndef LCC_INTERNAL_H
#define LCC_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "lcc/can.h"
#include "lcc/node.h"

/* Shared between the library's modules; not part of the public API. */

extern lcc_node_config_t lcc_config;

/* Time of the current call into the library, for timers started by sends */
extern uint32_t lcc_now;

/* Deadlines use a wrapping millisecond clock. */
static inline bool lcc_time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static inline uint32_t lcc_time_left(uint32_t now, uint32_t deadline)
{
    return lcc_time_reached(now, deadline) ? 0 : deadline - now;
}

/* Fold one deadline into a running "ms until next timer" minimum. */
static inline void lcc_timer_min(uint32_t *next, bool armed, uint32_t now,
                                 uint32_t deadline)
{
    if (armed) {
        uint32_t left = lcc_time_left(now, deadline);
        if (left < *next)
            *next = left;
    }
}

void lcc_send_frame(const lcc_frame_t *frame);
void lcc_send_initialization_complete(void);
void lcc_release_alias(void);
void lcc_send_global(uint16_t mti, const uint8_t *data, uint8_t len);
void lcc_send_addressed(uint16_t mti, uint16_t dst_alias,
                        const uint8_t *data, uint8_t len);

/* events.c */
void lcc_events_reset(void);
void lcc_events_identify_all(void);
bool lcc_events_handle(uint16_t mti, const uint8_t *data, uint8_t len);

/* datagram.c */
void lcc_datagram_reset(void);
void lcc_datagram_frame(const lcc_frame_t *frame, uint32_t now);
bool lcc_datagram_message(uint16_t mti, uint16_t src, const uint8_t *data,
                          uint8_t len, uint32_t now);
void lcc_datagram_alias_lost(void);
void lcc_datagram_run_timers(uint32_t now);
uint32_t lcc_datagram_next_timer(uint32_t now);
bool lcc_datagram_has_handlers(void);

/* memconfig.c */
void lcc_memconfig_reset(void);
void lcc_memconfig_run_timers(uint32_t now);
uint32_t lcc_memconfig_next_timer(uint32_t now);
bool lcc_memconfig_has_space(uint8_t space);
bool lcc_memconfig_has_spaces(void);
bool lcc_memconfig_has_firmware_upgrade(void);

#endif /* LCC_INTERNAL_H */
