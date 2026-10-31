// Runs signals.c on the real LCC library, recording what it asks the WS2811
// driver to show: aspects, flashing, lamps on outputs and which events are on
// the bus.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app/config.h"
#include "app/signals.h"
#include "lcc/bytes.h"
#include "lcc/events.h"
#include "lcc/node.h"
#include "lcc_task.h"
#include "timers.h"
#include "ws2811.h"

static int verbose;
void dbg_printf(const char *fmt, ...) { if (!verbose) return; va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }

// The LCC task runs calls straight away here
bool lcc_task_call(lcc_task_fn_t fn, uint32_t arg) { fn(arg); return true; }

config_layout_t config;
TimerCallbackFunction_t mock_timer_cb;
bool mock_timer_active;

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NODE_ID  0x050101011234ULL
#define NEW_ID   0x050101015678ULL
#define OTHER    0x0AB

// The WS2811 driver, recorded
static uint8_t chip[WS2811_CHIPS][3];
static int shows, chip_writes;
static uint32_t bit_hz;
void ws2811_set(int c, uint8_t r, uint8_t g, uint8_t b)
{
    chip[c][0] = r; chip[c][1] = g; chip[c][2] = b;
    chip_writes++;
}
void ws2811_show(void) { shows++; }
void ws2811_set_bit_rate(uint32_t hz) { bit_hz = hz; }

static lcc_frame_t sent[512];
static int nsent;
static uint32_t now = 1000;

static bool hook_send(const lcc_frame_t *f) { if (nsent < 512) sent[nsent++] = *f; return true; }

static bool has(uint16_t mti, uint64_t id)
{
    for (int i = 0; i < nsent; i++)
        if (!lcc_is_control_frame(sent[i].id) && sent[i].dlc == 8 &&
            lcc_frame_mti(sent[i].id) == mti && lcc_get_be64(sent[i].data) == id)
            return true;
    return false;
}

static int consumers_announced(void)
{
    int n = 0;
    for (int i = 0; i < nsent; i++) {
        if (lcc_is_control_frame(sent[i].id))
            continue;
        uint16_t mti = lcc_frame_mti(sent[i].id);
        n += mti == LCC_MTI_CONSUMER_IDENTIFIED_VALID || mti == LCC_MTI_CONSUMER_IDENTIFIED_INVALID ||
             mti == LCC_MTI_CONSUMER_IDENTIFIED_UNKNOWN;
    }
    return n;
}

static void rx(uint16_t mti, uint64_t id)
{
    lcc_frame_t f = { .id = lcc_message_id(mti, OTHER), .dlc = 8 };
    lcc_put_be64(f.data, id);
    nsent = 0;
    lcc_frame_received(&f, now);
}

static uint64_t event(int h, int aspect) { return lcc_get_be64(config.heads[h].aspect_events[aspect]); }
static void show(int h, int aspect) { rx(LCC_MTI_EVENT_REPORT, event(h, aspect)); }

static void identify_all(void)
{
    lcc_frame_t f = { .id = lcc_message_id(LCC_MTI_EVENTS_IDENTIFY_GLOBAL, OTHER) };
    nsent = 0;
    lcc_frame_received(&f, now);
    for (int i = 0; i < 16; i++) {  // any batches after the first
        now += 50;
        lcc_run_timers(now);
    }
}

static void flash(void) { mock_timer_cb(NULL); }

static bool chip_is(int c, uint8_t r, uint8_t g, uint8_t b)
{
    return chip[c][0] == r && chip[c][1] == g && chip[c][2] == b;
}

// Bring up a node, with the given heads enabled in the saved configuration
static void setup(int enabled_heads)
{
    lcc_node_config_t cfg = { .node_id = NODE_ID, .hooks = { .send = hook_send } };
    lcc_init(&cfg);
    memset(&config, 0, sizeof(config));
    signals_defaults(&config, NODE_ID);
    for (int h = 0; h < enabled_heads; h++)
        config.heads[h].enabled = 1;

    // signals.c keeps what it last applied: disable everything first
    config_layout_t want = config;
    for (int h = 0; h < CONFIG_HEADS; h++)
        config.heads[h].enabled = 0;
    mock_timer_active = false;
    signals_init();
    signals_config_written();
    config = want;
    signals_config_written();

    lcc_start(now);
    now += 200;
    nsent = 0;
    lcc_run_timers(now);
    shows = chip_writes = 0;
}

static void test_defaults(void)
{
    setup(0);
    CHECK(event(0, CONFIG_ASPECT_DARK) == (NODE_ID << 16 | 0x100) &&
          event(0, CONFIG_ASPECT_FLASHING_GREEN) == (NODE_ID << 16 | 0x10A) &&
          event(15, CONFIG_ASPECT_FLASHING_GREEN) == (NODE_ID << 16 | 0x1FA),
          "default events %016llx", (unsigned long long)event(15, CONFIG_ASPECT_FLASHING_GREEN));
    CHECK(bit_hz == WS2811_800KHZ, "bit rate %u", (unsigned)bit_hz);
    for (int c = 0; c < WS2811_CHIPS; c++)
        CHECK(chip_is(c, 0, 0, 0), "chip %d lit with no heads enabled", c);
    CHECK(!mock_timer_active, "flash timer running");
}

static void test_disabled_heads_stay_off_the_bus(void)
{
    setup(2);
    CHECK(consumers_announced() == 14, "announced %d events at startup, want 14", consumers_announced());
    identify_all();
    CHECK(consumers_announced() == 14, "identified %d events, want 14", consumers_announced());

    int writes = chip_writes;
    show(2, CONFIG_ASPECT_GREEN);
    CHECK(chip_writes == writes && chip_is(2, 0, 0, 0), "disabled head answered an event");

    rx(LCC_MTI_CONSUMER_IDENTIFY, event(2, CONFIG_ASPECT_GREEN));
    CHECK(nsent == 0, "disabled head answered an identify");

    // Every head enabled: all of them answer, through the identify batches
    setup(CONFIG_HEADS);
    identify_all();
    CHECK(consumers_announced() == CONFIG_HEADS * 7, "identified %d events", consumers_announced());
    show(15, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(15, 0, 128, 0), "last head didn't change");
}

static void test_aspects(void)
{
    setup(2);
    CHECK(chip_is(0, 128, 0, 0) && chip_is(1, 128, 0, 0), "not red at power-on");

    // Default wiring is R, G, Y on OUTR, OUTG, OUTB
    show(0, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(0, 0, 128, 0) && chip_is(1, 128, 0, 0), "green on head 1 only");
    CHECK(shows == 1, "%d frames for one aspect", shows);
    show(0, CONFIG_ASPECT_YELLOW);
    CHECK(chip_is(0, 0, 0, 128), "yellow");
    show(0, CONFIG_ASPECT_DARK);
    CHECK(chip_is(0, 0, 0, 0), "dark");
    CHECK(!mock_timer_active, "flash timer running for steady aspects");

    show(0, CONFIG_ASPECT_YELLOW);
    rx(LCC_MTI_CONSUMER_IDENTIFY, event(0, CONFIG_ASPECT_YELLOW));
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, event(0, CONFIG_ASPECT_YELLOW)), "yellow not valid");
    rx(LCC_MTI_CONSUMER_IDENTIFY, event(0, CONFIG_ASPECT_RED));
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_INVALID, event(0, CONFIG_ASPECT_RED)), "red not invalid");
    rx(LCC_MTI_CONSUMER_IDENTIFY, event(1, CONFIG_ASPECT_RED));
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, event(1, CONFIG_ASPECT_RED)), "head 2 not red");
}

static void set_lamps(int h, int r, int g, int b)
{
    config.heads[h].output_lamp[0] = (uint8_t)r;
    config.heads[h].output_lamp[1] = (uint8_t)g;
    config.heads[h].output_lamp[2] = (uint8_t)b;
    signals_config_written();
}

static void test_lamps_on_outputs(void)
{
    setup(1);
    config.heads[0].output_brightness[0] = 10;
    config.heads[0].output_brightness[1] = 20;
    config.heads[0].output_brightness[2] = 30;

    // Any lamp can be on any output, at that output's brightness
    set_lamps(0, CONFIG_LAMP_GREEN, CONFIG_LAMP_YELLOW, CONFIG_LAMP_RED);
    show(0, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(0, 10, 0, 0), "green on OUTR");
    show(0, CONFIG_ASPECT_YELLOW);
    CHECK(chip_is(0, 0, 20, 0), "yellow on OUTG");
    show(0, CONFIG_ASPECT_RED);
    CHECK(chip_is(0, 0, 0, 30), "red on OUTB");

    // A lamp on several outputs lights them all: the yard aspect's three yellows
    set_lamps(0, CONFIG_LAMP_YELLOW, CONFIG_LAMP_YELLOW, CONFIG_LAMP_YELLOW);
    show(0, CONFIG_ASPECT_YELLOW);
    CHECK(chip_is(0, 10, 20, 30), "three yellows");

    // A brightness change shows at once
    config.heads[0].output_brightness[1] = 200;
    signals_config_written();
    CHECK(chip_is(0, 10, 200, 30), "brightness change not shown");

    // An out-of-range lamp is never lit
    set_lamps(0, 99, CONFIG_LAMP_YELLOW, 200);
    CHECK(chip_is(0, 0, 200, 0), "bad lamp value lit");
}

// NYC time signals: the lights shown with a main aspect are a head of their own
static void test_subway_lights(void)
{
    setup(2);
    set_lamps(1, CONFIG_LAMP_LUNAR, CONFIG_LAMP_S, CONFIG_LAMP_D);
    CHECK(chip_is(1, 0, 0, 0), "power-on red on a head without one should be dark");

    show(0, CONFIG_ASPECT_RED);
    show(1, CONFIG_ASPECT_LUNAR);
    CHECK(chip_is(0, 128, 0, 0) && chip_is(1, 128, 0, 0), "red with lunar under it");
    show(0, CONFIG_ASPECT_YELLOW);
    show(1, CONFIG_ASPECT_S);
    CHECK(chip_is(0, 0, 0, 128) && chip_is(1, 0, 128, 0), "yellow with S");
    show(1, CONFIG_ASPECT_D);
    CHECK(chip_is(1, 0, 0, 128), "D");
    show(1, CONFIG_ASPECT_DARK);
    CHECK(chip_is(1, 0, 0, 0), "extra lights off");

    set_lamps(1, CONFIG_LAMP_NUMBER, CONFIG_LAMP_NONE, CONFIG_LAMP_YELLOW);
    show(1, CONFIG_ASPECT_NUMBER);
    CHECK(chip_is(1, 128, 0, 0), "speed number");
    show(1, CONFIG_ASPECT_YELLOW);
    CHECK(chip_is(1, 0, 0, 128), "call-on yellow");

    // CBTC's flash green
    show(0, CONFIG_ASPECT_FLASHING_GREEN);
    CHECK(chip_is(0, 0, 128, 0) && mock_timer_active, "flash green lit");
    flash();
    CHECK(chip_is(0, 0, 0, 0) && chip_is(1, 0, 0, 128), "flash green dark, others steady");

    // The extra lights stay steady through a flash
    set_lamps(1, CONFIG_LAMP_LUNAR, CONFIG_LAMP_S, CONFIG_LAMP_D);
    for (int a = CONFIG_ASPECT_LUNAR; a <= CONFIG_ASPECT_D; a++) {
        show(1, a);
        flash();
        flash();
        CHECK(chip[1][a - CONFIG_ASPECT_LUNAR] == 128, "aspect %d", a);
    }
    set_lamps(1, CONFIG_LAMP_NUMBER, CONFIG_LAMP_NONE, CONFIG_LAMP_NONE);
    show(1, CONFIG_ASPECT_NUMBER);
    for (int i = 0; i < 2; i++) {
        flash();
        CHECK(chip_is(1, 128, 0, 0), "number flashed");
    }
}

// A turnout or track on the same board can set a head by sharing an event ID
static void test_events_from_this_node(void)
{
    setup(1);
    lcc_event_send_id(event(0, CONFIG_ASPECT_GREEN));
    lcc_run_timers(now);
    CHECK(chip_is(0, 0, 128, 0), "head ignored an event from its own node");
}

// Only the aspects a head has lamps for are on the bus, and that follows its wiring
static void test_events_follow_the_lamps(void)
{
    setup(1);
    set_lamps(0, CONFIG_LAMP_LUNAR, CONFIG_LAMP_NONE, CONFIG_LAMP_NONE);
    identify_all();
    CHECK(consumers_announced() == 2 && has(LCC_MTI_CONSUMER_IDENTIFIED_INVALID, event(0, CONFIG_ASPECT_LUNAR)),
          "lunar-only head: %d events", consumers_announced());

    int writes = chip_writes;
    show(0, CONFIG_ASPECT_GREEN);
    CHECK(chip_writes == writes, "acted on an aspect it has no lamp for");

    set_lamps(0, CONFIG_LAMP_LUNAR, CONFIG_LAMP_GREEN, CONFIG_LAMP_NONE);
    show(0, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(0, 0, 128, 0), "new lamp's event ignored");
}

static void test_flashing(void)
{
    setup(3);
    show(0, CONFIG_ASPECT_FLASHING_YELLOW);
    CHECK(mock_timer_active && chip_is(0, 0, 0, 128), "flashing starts lit");

    show(1, CONFIG_ASPECT_GREEN);
    shows = 0;
    flash();
    CHECK(chip_is(0, 0, 0, 0) && chip_is(1, 0, 128, 0) && shows == 1, "first half: head 1 dark, head 2 steady");
    flash();
    CHECK(chip_is(0, 0, 0, 128) && chip_is(1, 0, 128, 0), "second half: head 1 lit");

    // A second head joins the first's flash
    flash();
    show(2, CONFIG_ASPECT_FLASHING_RED);
    CHECK(chip_is(2, 0, 0, 0), "joined out of step");
    flash();
    CHECK(chip_is(0, 0, 0, 128) && chip_is(2, 128, 0, 0), "not flashing together");

    // A head leaving a flashing aspect ends up steady, and the timer stops
    // with the last flashing head, wherever the flash was
    show(0, CONFIG_ASPECT_RED);
    CHECK(mock_timer_active && chip_is(0, 128, 0, 0), "left flashing");
    flash();
    CHECK(chip_is(0, 128, 0, 0), "steady head flashed");
    show(2, CONFIG_ASPECT_RED);
    CHECK(!mock_timer_active && chip_is(2, 128, 0, 0), "timer still running");

    // The next flash starts lit even though the last ended dark
    show(0, CONFIG_ASPECT_FLASHING_GREEN);
    CHECK(chip_is(0, 0, 128, 0), "restarted dark");
}

static void test_enable_and_disable(void)
{
    setup(1);
    show(0, CONFIG_ASPECT_FLASHING_GREEN);

    // Disabled: dark, off the bus, and nothing left to flash
    config.heads[0].enabled = 0;
    signals_config_written();
    CHECK(chip_is(0, 0, 0, 0) && !mock_timer_active, "disabled head still showing");
    identify_all();
    CHECK(consumers_announced() == 0, "disabled head announced");

    // Enabled again: on its power-on aspect, and answering events
    config.heads[0].power_on_aspect = CONFIG_ASPECT_FLASHING_RED;
    config.heads[0].enabled = 1;
    signals_config_written();
    CHECK(chip_is(0, 128, 0, 0) && mock_timer_active, "not on its power-on aspect");
    show(0, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(0, 0, 128, 0) && !mock_timer_active, "re-enabled head ignored its event");

    // A later write doesn't send an enabled head back to its power-on aspect
    signals_config_written();
    CHECK(chip_is(0, 0, 128, 0), "config write changed the aspect");

    // An event ID changed while running takes effect at once
    lcc_put_be64(config.heads[0].aspect_events[CONFIG_ASPECT_RED], 0x0102030405060708ULL);
    rx(LCC_MTI_EVENT_REPORT, 0x0102030405060708ULL);
    CHECK(chip_is(0, 128, 0, 0), "new event ID ignored");

    // A saved power-on aspect from out of range shows as red
    config.heads[0].enabled = 0;
    signals_config_written();
    config.heads[0].power_on_aspect = 99;
    config.heads[0].enabled = 1;
    signals_config_written();
    rx(LCC_MTI_CONSUMER_IDENTIFY, event(0, CONFIG_ASPECT_RED));
    CHECK(chip_is(0, 128, 0, 0) && has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, event(0, CONFIG_ASPECT_RED)),
          "bad power-on aspect");
}

static void test_chain_speed(void)
{
    setup(1);
    config.chain_speed = CONFIG_CHAIN_400KHZ;
    shows = 0;
    signals_config_written();
    CHECK(bit_hz == WS2811_400KHZ && shows == 1, "chain speed not applied");
    config.chain_speed = CONFIG_CHAIN_800KHZ;
    signals_config_written();
    CHECK(bit_hz == WS2811_800KHZ, "chain speed not restored");
}

static void test_migrate_events(void)
{
    setup(0);
    lcc_put_be64(config.heads[3].aspect_events[2], 0x0102030405060708ULL);  // user's own
    signals_migrate_events(&config, NODE_ID, NEW_ID);
    CHECK(event(0, CONFIG_ASPECT_DARK) == (NEW_ID << 16 | 0x100), "default event not moved");
    CHECK(event(15, 10) == (NEW_ID << 16 | 0x1FA), "last default event not moved");
    CHECK(event(3, 2) == 0x0102030405060708ULL, "user's event moved");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    test_defaults();
    test_disabled_heads_stay_off_the_bus();
    test_aspects();
    test_lamps_on_outputs();
    test_subway_lights();
    test_events_from_this_node();
    test_events_follow_the_lamps();
    test_flashing();
    test_enable_and_disable();
    test_chain_speed();
    test_migrate_events();
    printf("signals: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
