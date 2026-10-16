// Runs railcom_app.c on the real LCC library and checks the events it puts
// on the bus: occupied/clear pairs and DCC detector locomotive reports.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app/config.h"
#include "app/railcom_app.h"
#include "lcc/bytes.h"
#include "lcc/node.h"
#include "lcc_task.h"
#include "railcom.h"

static int verbose;
void dbg_printf(const char *fmt, ...) { if (!verbose) return; va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }

// The LCC task runs calls straight away here
bool lcc_task_call(lcc_task_fn_t fn, uint32_t arg) { fn(arg); return true; }

config_layout_t config;

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NODE_ID  0x050101011234ULL
#define OTHER    0x0AB
#define BASE1    (0x050101011235ULL << 16)  // track 1 reporter (bit 16 set)
#define BASE2    (0x050101011236ULL << 16)  // track 2 reporter (bit 16 clear)

static lcc_frame_t sent[256];
static int nsent;
static uint32_t now = 1000;

static bool hook_send(const lcc_frame_t *f) { if (nsent < 256) sent[nsent++] = *f; return true; }

// Global messages sent since the log was cleared: MTI and 8-byte payload
typedef struct { uint16_t mti; uint64_t id; } msg_t;
static int messages(msg_t *out)
{
    int n = 0;
    for (int i = 0; i < nsent; i++) {
        if (lcc_is_control_frame(sent[i].id) || sent[i].dlc != 8)
            continue;
        out[n++] = (msg_t){ lcc_frame_mti(sent[i].id), lcc_get_be64(sent[i].data) };
    }
    return n;
}

static bool has(uint16_t mti, uint64_t id)
{
    msg_t m[64];
    int n = messages(m);
    for (int i = 0; i < n; i++)
        if (m[i].mti == mti && m[i].id == id)
            return true;
    return false;
}

static int count(uint16_t mti)
{
    msg_t m[64];
    int n = messages(m), c = 0;
    for (int i = 0; i < n; i++)
        c += m[i].mti == mti;
    return c;
}

static void report(int track, bool occupied, uint16_t address, railcom_addr_type_t type,
                   railcom_orient_t orient)
{
    railcom_data_t d = { .address = address, .occupied = occupied, .addr_type = type,
                         .orientation = orient };
    nsent = 0;
    railcom_on_data(track, &d);
}

static void identify_producer(uint64_t id)
{
    lcc_frame_t f = { .id = lcc_message_id(LCC_MTI_PRODUCER_IDENTIFY, OTHER), .dlc = 8 };
    lcc_put_be64(f.data, id);
    nsent = 0;
    lcc_frame_received(&f, now);
}

static uint64_t occupied_event(int t) { return lcc_get_be64(config.tracks[t].occupied_event); }
static uint64_t clear_event(int t) { return lcc_get_be64(config.tracks[t].clear_event); }

static void setup(void)
{
    lcc_node_config_t cfg = { .node_id = NODE_ID, .hooks = { .send = hook_send } };
    lcc_init(&cfg);
    memset(&config, 0, sizeof(config));
    railcom_app_defaults(&config, NODE_ID);
    railcom_app_init();
    lcc_start(now);
    now += 200;
    nsent = 0;
    lcc_run_timers(now);  // alias claimed, events announced
}

static void test_announce_and_defaults(void)
{
    setup();
    CHECK(occupied_event(0) == (NODE_ID << 16 | 0x20) && clear_event(3) == (NODE_ID << 16 | 0x27),
          "default events %016llx %016llx", (unsigned long long)occupied_event(0),
          (unsigned long long)clear_event(3));
    CHECK(count(LCC_MTI_PRODUCER_IDENTIFIED_UNKNOWN) == 8, "occupancy events not announced as unknown");
    CHECK(has(LCC_MTI_PRODUCER_RANGE_IDENTIFIED, BASE1) &&
          has(LCC_MTI_PRODUCER_RANGE_IDENTIFIED, BASE2 | 0xFFFF) &&
          count(LCC_MTI_PRODUCER_RANGE_IDENTIFIED) == 4, "reporter ranges not announced");

    identify_producer(BASE1 | 0x3803);
    CHECK(has(LCC_MTI_PRODUCER_IDENTIFIED_UNKNOWN, BASE1 | 0x3803),
          "unknown track answered with a state");
}

static void test_occupancy_and_reports(void)
{
    setup();

    // An empty track becomes known
    report(1, false, 0, RAILCOM_ADDR_SHORT, RAILCOM_ORIENT_UNKNOWN);
    CHECK(has(LCC_MTI_EVENT_REPORT, clear_event(1)) && count(LCC_MTI_EVENT_REPORT) == 1,
          "vacant track: %d reports", count(LCC_MTI_EVENT_REPORT));

    // Short address 3 arrives on track 1; orientation isn't reported by default
    report(0, true, 3, RAILCOM_ADDR_SHORT, RAILCOM_ORIENT_A);
    CHECK(has(LCC_MTI_EVENT_REPORT, BASE1 | 0xF803) && has(LCC_MTI_EVENT_REPORT, occupied_event(0)) &&
          count(LCC_MTI_EVENT_REPORT) == 2, "loco 3 entry: %d reports", count(LCC_MTI_EVENT_REPORT));

    identify_producer(occupied_event(0));
    CHECK(has(LCC_MTI_PRODUCER_IDENTIFIED_VALID, occupied_event(0)), "occupied not valid");
    identify_producer(clear_event(0));
    CHECK(has(LCC_MTI_PRODUCER_IDENTIFIED_INVALID, clear_event(0)), "clear not invalid");

    // The xx.00.00 query lists the locomotive, answers for itself, then the range
    identify_producer(BASE1);
    msg_t m[16];
    int n = messages(m);
    CHECK(n == 3 && m[0].mti == LCC_MTI_PRODUCER_IDENTIFIED_VALID && m[0].id == (BASE1 | 0xF803) &&
          m[1].mti == LCC_MTI_PRODUCER_IDENTIFIED_VALID && m[1].id == BASE1 &&
          m[2].mti == LCC_MTI_PRODUCER_RANGE_IDENTIFIED, "query reply wrong (%d messages)", n);

    identify_producer(BASE1 | 0x3803);  // loco 3 left?
    CHECK(has(LCC_MTI_PRODUCER_IDENTIFIED_INVALID, BASE1 | 0x3803), "exit of present loco");
    identify_producer(BASE1 | 0xF804);  // loco 4 here?
    CHECK(has(LCC_MTI_PRODUCER_IDENTIFIED_INVALID, BASE1 | 0xF804), "entry of absent loco");
    identify_producer(BASE1 | 0x7803);  // facing West? not reported
    CHECK(has(LCC_MTI_PRODUCER_IDENTIFIED_UNKNOWN, BASE1 | 0x7803), "orientation not reported");

    // Orientation turned on: the loco enters again, facing West
    config.tracks[0].orientation = CONFIG_ORIENT_RAIL_A;
    nsent = 0;
    railcom_app_config_written();
    CHECK(has(LCC_MTI_EVENT_REPORT, BASE1 | 0x7803) && count(LCC_MTI_EVENT_REPORT) == 1,
          "orientation setting: %d reports", count(LCC_MTI_EVENT_REPORT));
    nsent = 0;
    railcom_app_config_written();
    CHECK(count(LCC_MTI_EVENT_REPORT) == 0, "unchanged setting reported again");
    identify_producer(BASE1 | 0x7803);
    CHECK(has(LCC_MTI_PRODUCER_IDENTIFIED_VALID, BASE1 | 0x7803), "West not valid");
    identify_producer(BASE1 | 0xB803);
    CHECK(has(LCC_MTI_PRODUCER_IDENTIFIED_INVALID, BASE1 | 0xB803), "East not invalid");

    // Turned around: enters again facing East
    report(0, true, 3, RAILCOM_ADDR_SHORT, RAILCOM_ORIENT_B);
    CHECK(has(LCC_MTI_EVENT_REPORT, BASE1 | 0xB803) && count(LCC_MTI_EVENT_REPORT) == 1,
          "turned loco: %d reports", count(LCC_MTI_EVENT_REPORT));

    // Rail B wiring flips the meaning
    config.tracks[0].orientation = CONFIG_ORIENT_RAIL_B;
    nsent = 0;
    railcom_app_config_written();
    CHECK(has(LCC_MTI_EVENT_REPORT, BASE1 | 0x7803), "rail B: current out of TRACK_R is West");
    config.tracks[0].orientation = CONFIG_ORIENT_RAIL_A;
    railcom_app_config_written();

    // Another loco takes its place: exit, then entry; still occupied
    report(0, true, 1234, RAILCOM_ADDR_LONG, RAILCOM_ORIENT_A);
    n = messages(m);
    CHECK(n == 2 && m[0].id == (BASE1 | 0x3803) && m[1].id == (BASE1 | 0x44D2),
          "loco change: %d messages", n);

    // It leaves
    report(0, false, 1234, RAILCOM_ADDR_LONG, RAILCOM_ORIENT_A);
    n = messages(m);
    CHECK(n == 2 && m[0].id == (BASE1 | 0x04D2) && m[1].id == clear_event(0),
          "vacate: %d messages", n);
    identify_producer(BASE1 | 0x04D2);
    CHECK(has(LCC_MTI_PRODUCER_IDENTIFIED_VALID, BASE1 | 0x04D2), "exit of departed loco");
    identify_producer(BASE1);
    CHECK(count(LCC_MTI_PRODUCER_IDENTIFIED_VALID) == 1 && has(LCC_MTI_PRODUCER_IDENTIFIED_VALID, BASE1),
          "empty track listed a loco");

    // Consist address
    report(2, true, 25, RAILCOM_ADDR_CONSIST, RAILCOM_ORIENT_UNKNOWN);
    CHECK(has(LCC_MTI_EVENT_REPORT, (0x050101011237ULL << 16) | 0xF919), "consist report wrong");
}

static void test_reporter_off(void)
{
    setup();
    memset(config.tracks[0].reporter_id, 0, 8);
    report(0, true, 3, RAILCOM_ADDR_SHORT, RAILCOM_ORIENT_A);
    CHECK(count(LCC_MTI_EVENT_REPORT) == 1 && has(LCC_MTI_EVENT_REPORT, occupied_event(0)),
          "reporter off still reports: %d", count(LCC_MTI_EVENT_REPORT));
    nsent = 0;
    lcc_frame_t f = { .id = lcc_message_id(LCC_MTI_EVENTS_IDENTIFY_GLOBAL, OTHER), .dlc = 0 };
    lcc_frame_received(&f, now);
    CHECK(count(LCC_MTI_PRODUCER_RANGE_IDENTIFIED) == 3, "disabled range announced");
}

static void test_migrate(void)
{
    const uint64_t new_id = 0x050101015500ULL;
    memset(&config, 0, sizeof(config));
    railcom_app_defaults(&config, NODE_ID);
    lcc_put_be64(config.tracks[1].clear_event, 0x0102030405060708ULL);  // user's own
    railcom_app_migrate_events(&config, NODE_ID, new_id);
    CHECK(occupied_event(0) == (new_id << 16 | 0x20), "occupied event not moved");
    CHECK(clear_event(1) == 0x0102030405060708ULL, "user's event moved");
    CHECK(lcc_get_be64(config.tracks[3].reporter_id) == (new_id + 4) << 16, "reporter ID not moved");
}

int main(int argc, char **argv)
{
    (void)argv;
    verbose = argc > 1;
    test_announce_and_defaults();
    test_occupancy_and_reports();
    test_reporter_off();
    test_migrate();
    printf("railcom app: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
