#include "railcom_app.h"
#include <string.h>
#include "config.h"
#include "lcc/bytes.h"
#include "lcc/events.h"
#include "lcc_task.h"
#include "railcom.h"
#include "util/dbg.h"

_Static_assert(CONFIG_TRACKS == RAILCOM_MAX_CHANNELS, "one track per detector channel");

// Locomotive report suffix (low 16 bits of the event ID), from the OpenLCB
// DCC Detector Protocol working note: bits 15-14 say whether the locomotive
// entered (and which way it faces) or left, bits 13-0 are its address.
#define REPORT_EXIT          0x0000
#define REPORT_ENTRY_WEST    0x4000
#define REPORT_ENTRY_EAST    0x8000
#define REPORT_ENTRY         0xC000  // orientation unknown
#define REPORT_KIND_MASK     0xC000
#define REPORT_ADDRESS_MASK  0x3FFF
#define REPORT_SHORT         0x3800  // | 7-bit address; long addresses are 0-10239
#define REPORT_CONSIST       0x3900

static struct {
    bool known;     // reported at least once
    bool occupied;
    railcom_data_t loco;  // the one on the track, or the last one when clear
    uint16_t entry_sent;  // its entry report, which says which way it faces
} tracks[CONFIG_TRACKS];

static lcc_event_t occupied_events[CONFIG_TRACKS];
static lcc_event_t clear_events[CONFIG_TRACKS];
static lcc_producer_range_t reporters[CONFIG_TRACKS];

static const config_track_t *track_cfg(int t)
{
    return &config.tracks[t];
}

// ------------------------------------------------------------------
// Locomotive reports
// ------------------------------------------------------------------

static uint16_t address_code(const railcom_data_t *d)
{
    switch (d->addr_type) {
    case RAILCOM_ADDR_SHORT:   return REPORT_SHORT | (d->address & 0x7F);
    case RAILCOM_ADDR_CONSIST: return REPORT_CONSIST | (d->address & 0x7F);
    default:                   return d->address & REPORT_ADDRESS_MASK;
    }
}

// The entry kind for a locomotive's orientation. The detector knows whether
// the decoder drove current into the TRACK_R rail (A) or out of it (B);
// current into Rail A means the locomotive faces West.
static uint16_t entry_kind(int t, railcom_orient_t orient)
{
    uint8_t rail = track_cfg(t)->orientation;
    if (orient == RAILCOM_ORIENT_UNKNOWN ||
        (rail != CONFIG_ORIENT_RAIL_A && rail != CONFIG_ORIENT_RAIL_B))
        return REPORT_ENTRY;
    bool into_rail_a = (orient == RAILCOM_ORIENT_A) == (rail == CONFIG_ORIENT_RAIL_A);
    return into_rail_a ? REPORT_ENTRY_WEST : REPORT_ENTRY_EAST;
}

static uint64_t reporter_base(int t)
{
    return lcc_get_be64(track_cfg(t)->reporter_id) & ~(uint64_t)0xFFFF;
}

static void send_report(int t, uint16_t suffix)
{
    uint64_t base = reporter_base(t);
    if (base)
        lcc_event_send_id(base | suffix);
}

// Report the locomotive on the track as entering, unless that report (with
// its orientation, which depends on the configuration too) already went out
static void send_entry(int t)
{
    uint16_t entry = entry_kind(t, tracks[t].loco.orientation) | address_code(&tracks[t].loco);
    if (entry != tracks[t].entry_sent) {
        tracks[t].entry_sent = entry;
        send_report(t, entry);
    }
}

// Identify Producer for an event in a track's range (draft rules for the
// working note, openlcb/documents PR 196). Asked about xx.00.00 ("long
// address 0 left"), a detector lists the locomotives present first.
static void reporter_identify(uintptr_t t, uint64_t event_id)
{
    uint16_t suffix = event_id & 0xFFFF;
    bool present = tracks[t].known && tracks[t].occupied;

    if (suffix == 0 && present) {
        uint16_t code = address_code(&tracks[t].loco);
        lcc_producer_send_identified(reporter_base(t) | entry_kind(t, tracks[t].loco.orientation) | code,
                                     LCC_EVENT_VALID);
    }

    lcc_event_state_t state;
    uint16_t kind = suffix & REPORT_KIND_MASK;
    bool here = present && (suffix & REPORT_ADDRESS_MASK) == address_code(&tracks[t].loco);
    if (!tracks[t].known) {
        state = LCC_EVENT_UNKNOWN;
    } else if (kind == REPORT_EXIT) {
        state = here ? LCC_EVENT_INVALID : LCC_EVENT_VALID;
    } else if (kind == REPORT_ENTRY) {
        state = here ? LCC_EVENT_VALID : LCC_EVENT_INVALID;
    } else if (entry_kind(t, RAILCOM_ORIENT_A) == REPORT_ENTRY) {
        state = LCC_EVENT_UNKNOWN;  // orientation isn't reported
    } else if (!here) {
        state = LCC_EVENT_INVALID;
    } else {
        uint16_t facing = entry_kind(t, tracks[t].loco.orientation);
        state = facing == REPORT_ENTRY ? LCC_EVENT_UNKNOWN
              : facing == kind         ? LCC_EVENT_VALID : LCC_EVENT_INVALID;
    }
    lcc_producer_send_identified(event_id, state);
}

// ------------------------------------------------------------------
// Detector reports, in the LCC task
// ------------------------------------------------------------------

// railcom_data_t packed into a task-call argument
#define PACK_CHANNEL(p)   ((p) & 0x3)
#define PACK_OCCUPIED     (1u << 2)
#define PACK_TYPE(p)      (((p) >> 3) & 0x3)
#define PACK_ORIENT(p)    (((p) >> 5) & 0x3)
#define PACK_ADDRESS(p)   ((p) >> 16)

static void data_in_lcc_task(uint32_t p)
{
    static const char *const type_name[] = { "short", "long", "consist" };
    static const char *const orient_name[] = { "?", "A", "B" };

    int t = (int)PACK_CHANNEL(p);
    railcom_data_t d = {
        .address = (uint16_t)PACK_ADDRESS(p),
        .occupied = (p & PACK_OCCUPIED) != 0,
        .addr_type = (railcom_addr_type_t)PACK_TYPE(p),
        .orientation = (railcom_orient_t)PACK_ORIENT(p),
    };
    DBG("RailCom T%d: %s loco=%u (%s, %s)\n", t + 1, d.occupied ? "occupied" : "vacant",
        d.address, type_name[d.addr_type], orient_name[d.orientation]);

    bool was_known = tracks[t].known, was_occupied = tracks[t].occupied;
    railcom_data_t was = tracks[t].loco;
    const config_track_t *c = track_cfg(t);

    tracks[t].known = true;
    tracks[t].occupied = d.occupied;
    if (d.occupied)
        tracks[t].loco = d;

    if (d.occupied) {
        bool same_loco = was_occupied && address_code(&was) == address_code(&d);
        if (was_occupied && !same_loco)
            send_report(t, REPORT_EXIT | address_code(&was));
        if (!same_loco)
            tracks[t].entry_sent = REPORT_EXIT;  // not an entry: none sent yet
        send_entry(t);  // a turned locomotive enters again, facing the other way
        if (!was_known || !was_occupied)
            lcc_event_send(c->occupied_event);
    } else {
        if (was_occupied)
            send_report(t, REPORT_EXIT | address_code(&was));
        if (!was_known || was_occupied)
            lcc_event_send(c->clear_event);
    }
}

static void signal_in_lcc_task(uint32_t p)
{
    if ((p & 0xFF) == 0)
        DBG("RailCom: cutouts %s\n", (p >> 8) ? "arriving" : "stopped, holding track states");
}

void railcom_on_data(int channel, railcom_data_t *data)
{
    uint32_t p = (uint32_t)channel | (data->occupied ? PACK_OCCUPIED : 0) |
                 ((uint32_t)data->addr_type << 3) | ((uint32_t)data->orientation << 5) |
                 ((uint32_t)data->address << 16);
    lcc_task_call(data_in_lcc_task, p);
}

void railcom_on_signal(int channel, bool active)
{
    lcc_task_call(signal_in_lcc_task, (uint32_t)channel | (active ? 0x100u : 0));
}

// ------------------------------------------------------------------
// Occupied / Clear
// ------------------------------------------------------------------

static lcc_event_state_t occupancy_state(uintptr_t t, bool occupied)
{
    if (!tracks[t].known)
        return LCC_EVENT_UNKNOWN;
    return tracks[t].occupied == occupied ? LCC_EVENT_VALID : LCC_EVENT_INVALID;
}

static lcc_event_state_t occupied_state(uintptr_t t) { return occupancy_state(t, true); }
static lcc_event_state_t clear_state(uintptr_t t) { return occupancy_state(t, false); }

// ------------------------------------------------------------------
// Configuration
// ------------------------------------------------------------------

// Default event IDs: the node ID followed by a 16-bit index; the switch
// machines use 0x00-0x13 and the signal heads 0x100-0x1FF. Default reporter IDs are the unique IDs after the
// node ID, so node IDs should be at least five apart.
static uint64_t default_occupied(uint64_t node_id, int t) { return node_id << 16 | (0x20 + 2 * t); }
static uint64_t default_clear(uint64_t node_id, int t) { return node_id << 16 | (0x21 + 2 * t); }
static uint64_t default_reporter(uint64_t node_id, int t)
{
    return ((node_id + 1 + t) & 0xFFFFFFFFFFFFULL) << 16;
}

void railcom_app_defaults(config_layout_t *cfg, uint64_t node_id)
{
    for (int t = 0; t < CONFIG_TRACKS; t++) {
        config_track_t *c = &cfg->tracks[t];
        memset(c, 0, sizeof(*c));
        lcc_put_be64(c->occupied_event, default_occupied(node_id, t));
        lcc_put_be64(c->clear_event, default_clear(node_id, t));
        lcc_put_be64(c->reporter_id, default_reporter(node_id, t));
        c->orientation = CONFIG_ORIENT_OFF;
    }
}

static void migrate(uint8_t *field, uint64_t old_value, uint64_t new_value)
{
    if (lcc_get_be64(field) == old_value)
        lcc_put_be64(field, new_value);
}

void railcom_app_migrate_events(config_layout_t *cfg, uint64_t old_id, uint64_t new_id)
{
    for (int t = 0; t < CONFIG_TRACKS; t++) {
        config_track_t *c = &cfg->tracks[t];
        migrate(c->occupied_event, default_occupied(old_id, t), default_occupied(new_id, t));
        migrate(c->clear_event, default_clear(old_id, t), default_clear(new_id, t));
        migrate(c->reporter_id, default_reporter(old_id, t), default_reporter(new_id, t));
    }
}

void railcom_app_config_written(void)
{
    // An orientation setting changed: present locomotives enter again
    for (int t = 0; t < CONFIG_TRACKS; t++) {
        if (tracks[t].known && tracks[t].occupied)
            send_entry(t);
    }
}

void railcom_app_init(void)
{
    for (int t = 0; t < CONFIG_TRACKS; t++) {
        config_track_t *c = &config.tracks[t];
        occupied_events[t] = (lcc_event_t){ .id = c->occupied_event, .producer = true,
                                            .state = occupied_state, .arg = t };
        clear_events[t] = (lcc_event_t){ .id = c->clear_event, .producer = true,
                                         .state = clear_state, .arg = t };
        reporters[t] = (lcc_producer_range_t){ .base = c->reporter_id, .bits = 16,
                                               .identify = reporter_identify, .arg = t };
        lcc_event_register(&occupied_events[t]);
        lcc_event_register(&clear_events[t]);
        lcc_producer_range_register(&reporters[t]);
    }
}
