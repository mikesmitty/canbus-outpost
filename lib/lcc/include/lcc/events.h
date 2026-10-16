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

#ifndef LCC_MAX_PRODUCER_RANGES
#define LCC_MAX_PRODUCER_RANGES  4
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

/*
 * A block of 2^bits event IDs this node produces, all sharing the base's
 * upper bits; the lower bits carry data (a DCC detector puts a loco
 * address there, for example). The library advertises it with Producer
 * Range Identified.
 */
typedef struct {
    /* 8-byte big-endian; the low `bits` bits are ignored. Read on every
     * use, like lcc_event_t.id. A base whose upper bits are all zero is
     * skipped. */
    const uint8_t *base;
    uint8_t bits;                             /* 1-32 */
    /* Identify Producer for an ID in the range: reply with
     * lcc_producer_send_identified() (at least for event_id itself). The
     * library follows it with Producer Range Identified. NULL = reply
     * Unknown. */
    void (*identify)(uintptr_t arg, uint64_t event_id);
    uintptr_t arg;
} lcc_producer_range_t;

/* Add a producer range. The struct must stay valid; returns false when full. */
bool lcc_producer_range_register(const lcc_producer_range_t *range);

/* Send Producer Identified Valid/Invalid/Unknown for one event ID. */
void lcc_producer_send_identified(uint64_t event_id, lcc_event_state_t state);

/* Send an Event Report. */
void lcc_event_send(const uint8_t *id);
void lcc_event_send_id(uint64_t event_id);

#endif /* LCC_EVENTS_H */
