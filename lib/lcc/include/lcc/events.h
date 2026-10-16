#ifndef LCC_EVENTS_H
#define LCC_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Event Transport: a registry of the events this node produces and
 * consumes. The library answers identify requests from it, announces it
 * after initialization and calls consumers when their events arrive.
 */

#ifndef LCC_MAX_EVENTS
#define LCC_MAX_EVENTS  48
#endif

typedef enum {
    LCC_EVENT_UNKNOWN,
    LCC_EVENT_VALID,
    LCC_EVENT_INVALID,
} lcc_event_state_t;

typedef struct {
    /* 8-byte big-endian event ID, read on every use so it can live in
     * configuration memory. An all-zero ID is skipped. */
    const uint8_t *id;
    bool producer;                            /* false = consumer */
    lcc_event_state_t (*state)(uintptr_t arg); /* NULL = always unknown */
    void (*on_event)(uintptr_t arg);          /* consumers: event received */
    uintptr_t arg;
} lcc_event_t;

/* Add an event. The struct must stay valid; returns false when full. */
bool lcc_event_register(const lcc_event_t *event);

/* Send an Event Report. */
void lcc_event_send(const uint8_t *id);
void lcc_event_send_id(uint64_t event_id);

#endif /* LCC_EVENTS_H */
