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
struct mock_timer mock_timers[MOCK_TIMERS];
int mock_ntimers;

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
static int connector = -1;
void ws2811_set(int c, uint8_t r, uint8_t g, uint8_t y)
{
    chip[c][0] = r; chip[c][1] = g; chip[c][2] = y;
    chip_writes++;
}
void ws2811_show(void) { shows++; }
void ws2811_set_bit_rate(uint32_t hz) { bit_hz = hz; }
void ws2811_set_connector(int c) { connector = c; }

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
static uint64_t lamp_event(int h, int out, bool on)
{
    return lcc_get_be64(on ? config.head_lamps[h].outputs[out].on_event
                           : config.head_lamps[h].outputs[out].off_event);
}
static void lamp(int h, int out, bool on) { rx(LCC_MTI_EVENT_REPORT, lamp_event(h, out, on)); }

static uint64_t test_event(bool start)
{
    return lcc_get_be64(start ? config.test_start_event : config.test_stop_event);
}

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

static void fire(const char *name) { TimerHandle_t t = mock_timer(name); t->cb(t); }
static bool running(const char *name) { return mock_timer(name)->active; }
static void fade_ticks(int n);
// Flash ticks are a twelfth of a second
static void flash_ticks(int n) { while (n-- > 0) fire("flash"); }
static void flash(void) { flash_ticks(6); }  // half a cycle at 60 a minute

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
    for (int i = 0; i < mock_ntimers; i++)
        mock_timers[i].active = false;
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
    CHECK(test_event(true) == (NODE_ID << 16 | 0x200) && test_event(false) == (NODE_ID << 16 | 0x201),
          "default test events");
    CHECK(lamp_event(0, 0, true) == (NODE_ID << 16 | 0x300) && lamp_event(0, 0, false) == (NODE_ID << 16 | 0x301) &&
          lamp_event(0, 2, false) == (NODE_ID << 16 | 0x305) && lamp_event(15, 2, false) == (NODE_ID << 16 | 0x37D),
          "default own events %016llx", (unsigned long long)lamp_event(15, 2, false));
    CHECK(config.heads[0].flash_rate == CONFIG_FLASH_MEDIUM && config.heads[0].output_phase[1] == CONFIG_PHASE_A,
          "default flashing");
    CHECK(bit_hz == WS2811_800KHZ, "bit rate %u", (unsigned)bit_hz);
    for (int c = 0; c < WS2811_CHIPS; c++)
        CHECK(chip_is(c, 0, 0, 0), "chip %d lit with no heads enabled", c);
    CHECK(!running("flash"), "flash timer running");
}

static void test_disabled_heads_stay_off_the_bus(void)
{
    setup(2);
    // 7 per head, plus the signal test's start and stop
    CHECK(consumers_announced() == 16, "announced %d events at startup, want 16", consumers_announced());
    identify_all();
    CHECK(consumers_announced() == 16, "identified %d events, want 16", consumers_announced());

    int writes = chip_writes;
    show(2, CONFIG_ASPECT_GREEN);
    CHECK(chip_writes == writes && chip_is(2, 0, 0, 0), "disabled head answered an event");

    rx(LCC_MTI_CONSUMER_IDENTIFY, event(2, CONFIG_ASPECT_GREEN));
    CHECK(nsent == 0, "disabled head answered an identify");

    // Every head enabled: all of them answer, through the identify batches
    setup(CONFIG_HEADS);
    identify_all();
    CHECK(consumers_announced() == CONFIG_HEADS * 7 + 2, "identified %d events", consumers_announced());
    show(15, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(15, 0, 128, 0), "last head didn't change");
}

static void test_aspects(void)
{
    setup(2);
    CHECK(chip_is(0, 128, 0, 0) && chip_is(1, 128, 0, 0), "not red at power-on");

    // Default lamps: red, green and yellow on outputs R, G and Y
    show(0, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(0, 0, 128, 0) && chip_is(1, 128, 0, 0), "green on head 1 only");
    CHECK(shows == 1, "%d frames for one aspect", shows);
    show(0, CONFIG_ASPECT_YELLOW);
    CHECK(chip_is(0, 0, 0, 128), "yellow");
    show(0, CONFIG_ASPECT_DARK);
    CHECK(chip_is(0, 0, 0, 0), "dark");
    CHECK(!running("flash"), "flash timer running for steady aspects");

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
    CHECK(chip_is(0, 10, 0, 0), "green on R");
    show(0, CONFIG_ASPECT_YELLOW);
    CHECK(chip_is(0, 0, 20, 0), "yellow on G");
    show(0, CONFIG_ASPECT_RED);
    CHECK(chip_is(0, 0, 0, 30), "red on Y");

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
    CHECK(chip_is(0, 0, 128, 0) && running("flash"), "flash green lit");
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
    CHECK(consumers_announced() == 2 + 2 && has(LCC_MTI_CONSUMER_IDENTIFIED_INVALID, event(0, CONFIG_ASPECT_LUNAR)),
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
    CHECK(running("flash") && chip_is(0, 0, 0, 128), "flashing starts lit");

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
    CHECK(running("flash") && chip_is(0, 128, 0, 0), "left flashing");
    flash();
    CHECK(chip_is(0, 128, 0, 0), "steady head flashed");
    show(2, CONFIG_ASPECT_RED);
    CHECK(!running("flash") && chip_is(2, 128, 0, 0), "timer still running");

    // The next flash starts lit even though the last ended dark
    show(0, CONFIG_ASPECT_FLASHING_GREEN);
    CHECK(chip_is(0, 0, 128, 0), "restarted dark");
}

// Each head flashes at its own rate; heads at one rate stay together
static void test_flash_rates(void)
{
    setup(3);
    config.heads[0].flash_rate = CONFIG_FLASH_SLOW;
    config.heads[2].flash_rate = CONFIG_FLASH_FAST;
    signals_config_written();
    show(0, CONFIG_ASPECT_FLASHING_RED);
    show(1, CONFIG_ASPECT_FLASHING_RED);
    show(2, CONFIG_ASPECT_FLASHING_RED);

    // Lit for 9, 6 and 4 ticks, then dark as long
    bool ok = true;
    for (int t = 0; t < 72 && ok; t++) {
        ok = chip[0][0] == ((t / 9) % 2 ? 0 : 128) && chip[1][0] == ((t / 6) % 2 ? 0 : 128) &&
             chip[2][0] == ((t / 4) % 2 ? 0 : 128);
        CHECK(ok, "tick %d: slow %u, medium %u, fast %u", t, chip[0][0], chip[1][0], chip[2][0]);
        flash_ticks(1);
    }
    CHECK(chip_is(0, 128, 0, 0) && chip_is(1, 128, 0, 0) && chip_is(2, 128, 0, 0),
          "the rates don't line up after a whole cycle");

    // An unknown rate flashes at the medium one
    config.heads[0].flash_rate = 7;
    signals_config_written();
    for (int t = 0; t < 12; t++) {
        CHECK(chip[0][0] == chip[1][0], "tick %d: unknown rate %u, medium %u", t, chip[0][0], chip[1][0]);
        flash_ticks(1);
    }
}

// Phase B lamps are lit while phase A lamps are dark: a crossing flasher on
// one head, or on two
static void test_flash_phases(void)
{
    setup(3);
    set_lamps(0, CONFIG_LAMP_RED, CONFIG_LAMP_RED, CONFIG_LAMP_NONE);
    config.heads[0].output_phase[1] = CONFIG_PHASE_B;
    config.heads[2].output_phase[0] = CONFIG_PHASE_B;
    signals_config_written();

    show(0, CONFIG_ASPECT_FLASHING_RED);
    show(1, CONFIG_ASPECT_FLASHING_RED);
    show(2, CONFIG_ASPECT_FLASHING_RED);
    CHECK(chip_is(0, 128, 0, 0) && chip_is(1, 128, 0, 0) && chip_is(2, 0, 0, 0), "first half");
    flash();
    CHECK(chip_is(0, 0, 128, 0) && chip_is(1, 0, 0, 0) && chip_is(2, 128, 0, 0), "second half");
    flash();
    CHECK(chip_is(0, 128, 0, 0) && chip_is(1, 128, 0, 0) && chip_is(2, 0, 0, 0), "third half");

    // A steady aspect ignores the phase
    show(2, CONFIG_ASPECT_RED);
    flash();
    CHECK(chip_is(2, 128, 0, 0), "steady red flashed");
    flash();
    CHECK(chip_is(2, 128, 0, 0), "steady red went dark");
}

// An output set to Own Events follows its On and Off events, beside the
// aspects on the head's other outputs
static void test_own_events(void)
{
    setup(2);
    config.heads[0].output_brightness[2] = 40;
    set_lamps(0, CONFIG_LAMP_RED, CONFIG_LAMP_GREEN, CONFIG_LAMP_OWN);
    CHECK(chip_is(0, 128, 0, 0), "not red");

    // On the bus only for the own output of the enabled head; it starts dark
    identify_all();
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_INVALID, lamp_event(0, 2, true)) &&
          has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, lamp_event(0, 2, false)), "own events not identified");
    CHECK(!has(LCC_MTI_CONSUMER_IDENTIFIED_INVALID, lamp_event(0, 0, true)) &&
          !has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, lamp_event(0, 0, false)) &&
          !has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, lamp_event(1, 2, false)), "other outputs' own events on the bus");
    // Head 1 lost yellow (5 aspects), head 2 has its 7, and the test's 2
    CHECK(consumers_announced() == 5 + 2 + 7 + 2, "identified %d events", consumers_announced());

    // Lit at its brightness, beside the aspect, and through aspect changes
    lamp(0, 2, true);
    CHECK(chip_is(0, 128, 0, 40), "own output not lit");
    show(0, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(0, 0, 128, 40), "own output followed the aspect");
    show(0, CONFIG_ASPECT_DARK);
    CHECK(chip_is(0, 0, 0, 40), "dark aspect put out the own output");
    rx(LCC_MTI_CONSUMER_IDENTIFY, lamp_event(0, 2, true));
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, lamp_event(0, 2, true)), "on not valid");
    rx(LCC_MTI_CONSUMER_IDENTIFY, lamp_event(0, 2, false));
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_INVALID, lamp_event(0, 2, false)), "off not invalid");
    lamp(0, 2, false);
    CHECK(chip_is(0, 0, 0, 0), "own output not out");

    // Aspects never light it
    set_lamps(0, CONFIG_LAMP_RED, CONFIG_LAMP_OWN, CONFIG_LAMP_OWN);
    show(0, CONFIG_ASPECT_RED);
    show(0, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(0, 128, 0, 0), "green event lit an own output");

    // Two lamps lit together on one chip, each at its brightness
    lamp(0, 1, true);
    lamp(0, 2, true);
    CHECK(chip_is(0, 128, 128, 40), "two own outputs");

    // Set to another lamp, it stops following its events and starts dark
    // when it's set back
    set_lamps(0, CONFIG_LAMP_RED, CONFIG_LAMP_GREEN, CONFIG_LAMP_OWN);
    CHECK(chip_is(0, 128, 0, 40), "G still lit as an own output");
    lamp(0, 1, true);
    CHECK(chip_is(0, 128, 0, 40), "G followed its own event as a green lamp");
    set_lamps(0, CONFIG_LAMP_RED, CONFIG_LAMP_OWN, CONFIG_LAMP_OWN);
    CHECK(chip_is(0, 128, 0, 40), "G came back lit");

    // Disabled: dark and off the bus; enabled again, dark
    config.heads[0].enabled = 0;
    signals_config_written();
    CHECK(chip_is(0, 0, 0, 0), "disabled head's own output lit");
    int writes = chip_writes;
    lamp(0, 2, true);
    CHECK(chip_writes == writes, "disabled head followed an own event");
    config.heads[0].enabled = 1;
    signals_config_written();
    CHECK(chip_is(0, 128, 0, 0), "enabled again: not just red");
}

// A head with only own outputs is no signal: none of its aspects are on the
// bus, Dark included
static void test_own_events_only(void)
{
    setup(1);
    set_lamps(0, CONFIG_LAMP_OWN, CONFIG_LAMP_OWN, CONFIG_LAMP_NONE);
    identify_all();
    CHECK(consumers_announced() == 2 * 2 + 2, "identified %d events", consumers_announced());
    CHECK(!has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, event(0, CONFIG_ASPECT_DARK)) &&
          !has(LCC_MTI_CONSUMER_IDENTIFIED_INVALID, event(0, CONFIG_ASPECT_DARK)), "dark on the bus");
    CHECK(chip_is(0, 0, 0, 0), "lit at power-on");

    // Fading like the head's lamps
    config.heads[0].lamp_fade = CONFIG_FADE_INCANDESCENT;
    signals_config_written();
    lamp(0, 0, true);
    CHECK(chip[0][0] < 128 && running("fade"), "didn't fade up: %u", chip[0][0]);
    fade_ticks(100);
    CHECK(chip_is(0, 128, 0, 0), "didn't reach full");

    // Lamps not on a head's own outputs still go through its events
    set_lamps(0, CONFIG_LAMP_OWN, CONFIG_LAMP_OWN, CONFIG_LAMP_YELLOW);
    identify_all();
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_INVALID, event(0, CONFIG_ASPECT_DARK)), "dark not back");
}

static void test_enable_and_disable(void)
{
    setup(1);
    show(0, CONFIG_ASPECT_FLASHING_GREEN);

    // Disabled: dark, off the bus, and nothing left to flash
    config.heads[0].enabled = 0;
    signals_config_written();
    CHECK(chip_is(0, 0, 0, 0) && !running("flash"), "disabled head still showing");
    identify_all();
    CHECK(consumers_announced() == 2, "disabled head announced");

    // Enabled again: on its power-on aspect, and answering events
    config.heads[0].power_on_aspect = CONFIG_ASPECT_FLASHING_RED;
    config.heads[0].enabled = 1;
    signals_config_written();
    CHECK(chip_is(0, 128, 0, 0) && running("flash"), "not on its power-on aspect");
    show(0, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(0, 0, 128, 0) && !running("flash"), "re-enabled head ignored its event");

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

static void test_connector(void)
{
    setup(1);
    CHECK(connector == CONFIG_CONNECTOR_LED, "default connector %d", connector);
    config.chain_connector = CONFIG_CONNECTOR_SERVO1 + 3;
    signals_config_written();
    CHECK(connector == 4, "chain not moved to servo 4: %d", connector);
    config.chain_connector = CONFIG_CONNECTOR_LED;
    signals_config_written();
    CHECK(connector == CONFIG_CONNECTOR_LED, "chain not back on the LED header: %d", connector);
}

static void fade_ticks(int n)
{
    for (int i = 0; i < n; i++)
        fire("fade");
}

static void test_fade(void)
{
    setup(2);
    config.heads[0].lamp_fade = CONFIG_FADE_INCANDESCENT;
    signals_config_written();
    CHECK(chip_is(0, 128, 0, 0) && !running("fade"), "turning fade on changed a steady lamp");

    // Red cools while green warms, both starting from where they were
    show(0, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(0, 128, 0, 0) && running("fade"), "changed before the first fade tick");

    // A head without fade still changes at once
    show(1, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(1, 0, 128, 0), "head without fade faded");

    int ticks = 0, half = -1;
    uint8_t red = 128, green = 0;
    bool smooth = true;
    while (chip[0][1] < 128 && ticks < 100) {
        fade_ticks(1);
        ticks++;
        smooth &= chip[0][0] <= red && chip[0][1] >= green;
        red = chip[0][0];
        green = chip[0][1];
        if (ticks == 8)
            half = green;
    }
    CHECK(smooth, "lamps didn't fade one way");
    CHECK(ticks >= 15 && ticks <= 16, "green took %d ticks to warm up", ticks);
    CHECK(half > 0 && half < 64, "halfway, green at %d: should be dim and rising", half);
    CHECK(chip[0][0] > 0 && running("fade"), "red cooled as fast as green warmed");

    fade_ticks(15);
    CHECK(chip_is(0, 0, 128, 0) && !running("fade"), "fade didn't finish: %u %u %u, timer %d",
          chip[0][0], chip[0][1], chip[0][2], running("fade"));

    // Flashing fades too: lit, then cooling once the flash goes dark
    show(0, CONFIG_ASPECT_FLASHING_YELLOW);
    fade_ticks(40);
    CHECK(chip_is(0, 0, 0, 128) && !running("fade"), "flashing yellow didn't warm up");
    flash();
    CHECK(chip_is(0, 0, 0, 128) && running("fade"), "flash went dark at once");
    fade_ticks(5);
    CHECK(chip[0][2] > 0 && chip[0][2] < 128, "flash not cooling: %u", chip[0][2]);

    // Turning fade off part way finishes at once
    config.heads[0].lamp_fade = CONFIG_FADE_NONE;
    signals_config_written();
    CHECK(chip_is(0, 0, 0, 0), "fade off didn't finish the fade");
    fade_ticks(1);
    CHECK(!running("fade"), "fade timer kept running");

    // A head enabled with fade warms up into its power-on aspect
    config.heads[2].lamp_fade = CONFIG_FADE_INCANDESCENT;
    config.heads[2].enabled = 1;
    signals_config_written();
    CHECK(chip_is(2, 0, 0, 0) && running("fade"), "power-on aspect didn't warm up");
    fade_ticks(40);
    CHECK(chip_is(2, 128, 0, 0), "power-on aspect not lit");
}

static void test_chain_position(void)
{
    setup(2);

    // Head 2 moved to the fourth chip: its own chip goes dark
    config.heads[1].chain_position = 4;
    signals_config_written();
    CHECK(chip_is(1, 0, 0, 0) && chip_is(3, 128, 0, 0), "head 2 not on chip 4");
    show(1, CONFIG_ASPECT_GREEN);
    CHECK(chip_is(3, 0, 128, 0) && chip_is(1, 0, 0, 0), "head 2's aspect not on chip 4");

    // Two heads on one chip both show
    config.heads[1].chain_position = 1;
    signals_config_written();
    CHECK(chip_is(0, 128, 128, 0) && chip_is(3, 0, 0, 0), "two heads on chip 1");

    // 0 and anything past the chain mean the head's own chip
    config.heads[1].chain_position = 17;
    signals_config_written();
    CHECK(chip_is(1, 0, 128, 0) && chip_is(0, 128, 0, 0), "position 17 not head 2's own chip");
    config.heads[1].chain_position = 0;
    signals_config_written();
    CHECK(chip_is(1, 0, 128, 0), "position 0 not head 2's own chip");

    // An event only rewrites the chips that changed, so a chip set by hand
    // (the USB lights command) keeps that until its own head changes
    chip[5][0] = 77;
    show(0, CONFIG_ASPECT_YELLOW);
    CHECK(chip[5][0] == 77, "chip set by hand overwritten by another head");
    signals_config_written();
    CHECK(chip_is(5, 0, 0, 0), "config write didn't rewrite every chip");
}

static void test_signal_test(void)
{
    setup(2);
    show(0, CONFIG_ASPECT_GREEN);
    config.heads[1].lamp_fade = CONFIG_FADE_INCANDESCENT;
    signals_config_written();

    // Every chip, enabled or not, steps through red, yellow, green (outputs
    // R, Y, G) and none
    signals_test(true);
    CHECK(signals_testing() && running("test"), "test not running");
    static const uint8_t want[5][3] = { {128, 0, 0}, {0, 0, 128}, {0, 128, 0}, {0, 0, 0}, {128, 0, 0} };
    for (int step = 0; step < 5; step++) {
        bool all = true;
        for (int c = 0; c < WS2811_CHIPS; c++)
            all &= chip_is(c, want[step][0], want[step][1], want[step][2]);
        CHECK(all, "step %d: chip 16 at %u %u %u", step, chip[15][0], chip[15][1], chip[15][2]);
        fire("test");
    }

    // The heads follow their events meanwhile, without touching the chain
    int writes = chip_writes;
    show(1, CONFIG_ASPECT_YELLOW);
    fade_ticks(40);
    config.heads[0].output_brightness[1] = 200;
    signals_config_written();
    CHECK(chip_writes == writes, "heads wrote to the chain during the test");
    rx(LCC_MTI_CONSUMER_IDENTIFY, event(1, CONFIG_ASPECT_YELLOW));
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, event(1, CONFIG_ASPECT_YELLOW)), "head 2 not yellow");

    // Afterwards every chip shows its head again
    signals_test(false);
    CHECK(!signals_testing() && !running("test"), "test still running");
    CHECK(chip_is(0, 0, 200, 0) && chip_is(1, 0, 0, 128) && chip_is(15, 0, 0, 0),
          "heads not back: %u %u %u / %u %u %u", chip[0][0], chip[0][1], chip[0][2],
          chip[1][0], chip[1][1], chip[1][2]);

    // A test step queued before the end changes nothing
    writes = chip_writes;
    fire("test");
    CHECK(chip_writes == writes, "test step after the test");

    // Over LCC: the start and stop events, and their state
    rx(LCC_MTI_EVENT_REPORT, test_event(true));
    CHECK(signals_testing() && chip_is(15, 128, 0, 0), "start event didn't start the test");
    rx(LCC_MTI_CONSUMER_IDENTIFY, test_event(true));
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, test_event(true)), "start not valid while testing");
    rx(LCC_MTI_EVENT_REPORT, test_event(true));
    CHECK(signals_testing() && chip_is(15, 128, 0, 0), "a second start restarted the test");
    rx(LCC_MTI_EVENT_REPORT, test_event(false));
    CHECK(!signals_testing() && chip_is(0, 0, 200, 0) && chip_is(15, 0, 0, 0), "stop event didn't stop it");
    rx(LCC_MTI_CONSUMER_IDENTIFY, test_event(true));
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_INVALID, test_event(true)), "start valid after the test");
    rx(LCC_MTI_CONSUMER_IDENTIFY, test_event(false));
    CHECK(has(LCC_MTI_CONSUMER_IDENTIFIED_VALID, test_event(false)), "stop not valid after the test");

    // A pair shared by every node, set while running, takes effect at once
    lcc_put_be64(config.test_start_event, 0x0102030405060700ULL);
    signals_config_written();
    rx(LCC_MTI_EVENT_REPORT, 0x0102030405060700ULL);
    CHECK(signals_testing(), "new start event ignored");
    signals_test(false);
}

static void test_migrate_events(void)
{
    setup(0);
    lcc_put_be64(config.heads[3].aspect_events[2], 0x0102030405060708ULL);  // user's own
    lcc_put_be64(config.head_lamps[3].outputs[1].off_event, 0x0102030405060709ULL);
    signals_migrate_events(&config, NODE_ID, NEW_ID);
    CHECK(event(0, CONFIG_ASPECT_DARK) == (NEW_ID << 16 | 0x100), "default event not moved");
    CHECK(event(15, 10) == (NEW_ID << 16 | 0x1FA), "last default event not moved");
    CHECK(event(3, 2) == 0x0102030405060708ULL, "user's event moved");
    CHECK(lamp_event(3, 1, false) == 0x0102030405060709ULL, "user's own event moved");
    CHECK(test_event(true) == (NEW_ID << 16 | 0x200) && test_event(false) == (NEW_ID << 16 | 0x201),
          "test events not moved");
    CHECK(lamp_event(0, 0, true) == (NEW_ID << 16 | 0x300) && lamp_event(15, 2, false) == (NEW_ID << 16 | 0x37D),
          "own events not moved");
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
    test_flash_rates();
    test_flash_phases();
    test_own_events();
    test_own_events_only();
    test_enable_and_disable();
    test_chain_speed();
    test_connector();
    test_fade();
    test_chain_position();
    test_signal_test();
    test_migrate_events();
    printf("signals: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
