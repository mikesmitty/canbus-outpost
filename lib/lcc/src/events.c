#include <string.h>
#include "internal.h"
#include "lcc/bytes.h"
#include "lcc/events.h"

static const lcc_event_t *events[LCC_MAX_EVENTS];
static int event_count;

static const uint8_t zero_id[8];

static bool id_used(const uint8_t *id)
{
    return memcmp(id, zero_id, 8) != 0;
}

static void send_identified(const lcc_event_t *ev)
{
    lcc_event_state_t state = ev->state ? ev->state(ev->arg) : LCC_EVENT_UNKNOWN;
    uint16_t mti;

    if (ev->producer) {
        mti = state == LCC_EVENT_VALID   ? LCC_MTI_PRODUCER_IDENTIFIED_VALID :
              state == LCC_EVENT_INVALID ? LCC_MTI_PRODUCER_IDENTIFIED_INVALID :
                                           LCC_MTI_PRODUCER_IDENTIFIED_UNKNOWN;
    } else {
        mti = state == LCC_EVENT_VALID   ? LCC_MTI_CONSUMER_IDENTIFIED_VALID :
              state == LCC_EVENT_INVALID ? LCC_MTI_CONSUMER_IDENTIFIED_INVALID :
                                           LCC_MTI_CONSUMER_IDENTIFIED_UNKNOWN;
    }
    lcc_send_global(mti, ev->id, 8);
}

void lcc_events_reset(void)
{
    event_count = 0;
}

bool lcc_event_register(const lcc_event_t *event)
{
    if (event_count >= LCC_MAX_EVENTS)
        return false;
    events[event_count++] = event;
    return true;
}

void lcc_events_identify_all(void)
{
    for (int i = 0; i < event_count; i++) {
        if (id_used(events[i]->id))
            send_identified(events[i]);
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
    } else if (mti == LCC_MTI_CONSUMER_IDENTIFY) {
        identify_matching(data, false);
    } else {
        for (int i = 0; i < event_count; i++) {
            const lcc_event_t *ev = events[i];
            if (!ev->producer && ev->on_event && memcmp(ev->id, data, 8) == 0)
                ev->on_event(ev->arg);
        }
    }
    return true;
}

void lcc_event_send(const uint8_t *id)
{
    if (lcc_is_initialized() && id_used(id))
        lcc_send_global(LCC_MTI_EVENT_REPORT, id, 8);
}

void lcc_event_send_id(uint64_t event_id)
{
    uint8_t id[8];
    lcc_put_be64(id, event_id);
    lcc_event_send(id);
}
