#include <string.h>
#include "internal.h"
#include "lcc/bytes.h"
#include "lcc/events.h"

static const lcc_event_t *events[LCC_MAX_EVENTS];
static int event_count;

static const lcc_producer_range_t *ranges[LCC_MAX_PRODUCER_RANGES];
static int range_count;

static const uint8_t zero_id[8];

/* An identify-all goes out a batch at a time: the send hook can block while
 * the bus is busy, and a node with many events still has other traffic to
 * handle (and a watchdog to feed) during a long one. */
#define IDENTIFY_BATCH     32
#define IDENTIFY_BATCH_MS  50

static int identify_pos;       /* next one to send: events, then ranges */
static bool identifying;       /* more to send */
static uint32_t identify_next;

/* A node doesn't hear its own frames, so its consumers get the events it
 * sends from here, on the next run of the timers. Not from inside
 * lcc_event_send: the caller may be part way through a change, and an event
 * wired back to itself mustn't recurse. A queue that fills up drops what
 * comes next, on this side only; the bus still gets those events. */
#define LOCAL_QUEUE_LEN  16

static uint8_t local_queue[LOCAL_QUEUE_LEN][8];
static int local_head, local_count;

static bool id_used(const uint8_t *id)
{
    return memcmp(id, zero_id, 8) != 0;
}

static uint16_t identified_mti(bool producer, lcc_event_state_t state)
{
    if (producer)
        return state == LCC_EVENT_VALID   ? LCC_MTI_PRODUCER_IDENTIFIED_VALID :
               state == LCC_EVENT_INVALID ? LCC_MTI_PRODUCER_IDENTIFIED_INVALID :
                                            LCC_MTI_PRODUCER_IDENTIFIED_UNKNOWN;
    return state == LCC_EVENT_VALID   ? LCC_MTI_CONSUMER_IDENTIFIED_VALID :
           state == LCC_EVENT_INVALID ? LCC_MTI_CONSUMER_IDENTIFIED_INVALID :
                                        LCC_MTI_CONSUMER_IDENTIFIED_UNKNOWN;
}

static void send_identified(const lcc_event_t *ev)
{
    lcc_event_state_t state = ev->state ? ev->state(ev->arg) : LCC_EVENT_UNKNOWN;
    lcc_send_global(identified_mti(ev->producer, state), ev->id, 8);
}

/* Range bases with the data bits masked off; 0 = range unused */
static uint64_t range_base(const lcc_producer_range_t *r)
{
    return lcc_get_be64(r->base) & ~((1ULL << r->bits) - 1);
}

/* Range identifiers mark the size with a run of equal low bits, which the
 * bit above must differ from. */
static void send_range_identified(const lcc_producer_range_t *r)
{
    uint64_t base = range_base(r);
    uint64_t mask = (1ULL << r->bits) - 1;
    uint64_t id = (base & (mask + 1)) ? base : base | mask;
    uint8_t buf[8];
    lcc_put_be64(buf, id);
    lcc_send_global(LCC_MTI_PRODUCER_RANGE_IDENTIFIED, buf, 8);
}

void lcc_events_reset(void)
{
    event_count = 0;
    range_count = 0;
    identifying = false;
    local_head = 0;
    local_count = 0;
}

bool lcc_event_register(const lcc_event_t *event)
{
    if (event_count >= LCC_MAX_EVENTS)
        return false;
    events[event_count++] = event;
    return true;
}

bool lcc_producer_range_register(const lcc_producer_range_t *range)
{
    if (range_count >= LCC_MAX_PRODUCER_RANGES || range->bits < 1 || range->bits > 32)
        return false;
    ranges[range_count++] = range;
    return true;
}

void lcc_producer_send_identified(uint64_t event_id, lcc_event_state_t state)
{
    uint8_t buf[8];
    lcc_put_be64(buf, event_id);
    lcc_send_global(identified_mti(true, state), buf, 8);
}

static void identify_batch(void)
{
    int total = event_count + range_count;
    for (int batch = 0; batch < IDENTIFY_BATCH && identify_pos < total; ) {
        int i = identify_pos++;
        if (i < event_count) {
            if (id_used(events[i]->id)) {
                send_identified(events[i]);
                batch++;
            }
        } else if (range_base(ranges[i - event_count])) {
            send_range_identified(ranges[i - event_count]);
            batch++;
        }
    }
    identifying = identify_pos < total;
    identify_next = lcc_now + IDENTIFY_BATCH_MS;
}

void lcc_events_identify_all(void)
{
    identify_pos = 0;
    identify_batch();
}

/* Run every consumer of an event */
static void consume(const uint8_t *id)
{
    for (int i = 0; i < event_count; i++) {
        const lcc_event_t *ev = events[i];
        if (!ev->producer && ev->on_event && memcmp(ev->id, id, 8) == 0)
            ev->on_event(ev->arg);
    }
}

void lcc_events_run_timers(uint32_t now)
{
    /* Only what's queued now: an event wired back to itself waits its turn,
     * so the caller's other work still gets a look in */
    for (int n = local_count; n > 0; n--) {
        uint8_t id[8];
        memcpy(id, local_queue[local_head], 8);
        local_head = (local_head + 1) % LOCAL_QUEUE_LEN;
        local_count--;
        consume(id);
    }

    if (identifying && lcc_time_reached(now, identify_next))
        identify_batch();
}

uint32_t lcc_events_next_timer(uint32_t now)
{
    uint32_t next = LCC_NO_TIMER;
    lcc_timer_min(&next, identifying, now, identify_next);
    lcc_timer_min(&next, local_count > 0, now, now);
    return next;
}

static void identify_in_ranges(uint64_t id)
{
    for (int i = 0; i < range_count; i++) {
        const lcc_producer_range_t *r = ranges[i];
        uint64_t base = range_base(r);
        if (!base || (id & ~((1ULL << r->bits) - 1)) != base)
            continue;
        if (r->identify)
            r->identify(r->arg, id);
        else
            lcc_producer_send_identified(id, LCC_EVENT_UNKNOWN);
        send_range_identified(r);
    }
}

/* Answer identify requests for one event ID, from producers or consumers. */
static void identify_matching(const uint8_t *id, bool producers)
{
    for (int i = 0; i < event_count; i++) {
        const lcc_event_t *ev = events[i];
        if (ev->producer == producers && memcmp(ev->id, id, 8) == 0)
            send_identified(ev);
    }
}

bool lcc_events_handle(uint16_t mti, const uint8_t *data, uint8_t len)
{
    switch (mti) {
    case LCC_MTI_EVENTS_IDENTIFY_GLOBAL:
        lcc_events_identify_all();
        return true;
    case LCC_MTI_PRODUCER_IDENTIFY:
    case LCC_MTI_CONSUMER_IDENTIFY:
    case LCC_MTI_EVENT_REPORT:
        break;
    default:
        return false;
    }

    if (len < 8 || !id_used(data))
        return true;

    if (mti == LCC_MTI_PRODUCER_IDENTIFY) {
        identify_matching(data, true);
        identify_in_ranges(lcc_get_be64(data));
    } else if (mti == LCC_MTI_CONSUMER_IDENTIFY) {
        identify_matching(data, false);
    } else {
        consume(data);
    }
    return true;
}

void lcc_event_send(const uint8_t *id)
{
    if (!lcc_is_initialized() || !id_used(id))
        return;
    lcc_send_global(LCC_MTI_EVENT_REPORT, id, 8);
    if (local_count < LOCAL_QUEUE_LEN)
        memcpy(local_queue[(local_head + local_count++) % LOCAL_QUEUE_LEN], id, 8);
}

void lcc_event_send_id(uint64_t event_id)
{
    uint8_t id[8];
    lcc_put_be64(id, event_id);
    lcc_event_send(id);
}
