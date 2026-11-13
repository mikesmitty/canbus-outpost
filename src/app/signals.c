#include "signals.h"
#include <string.h>
#include "FreeRTOS.h"
#include "timers.h"
#include "config.h"
#include "lcc/bytes.h"
#include "lcc/events.h"
#include "lcc_task.h"
#include "util/dbg.h"
#include "ws2811.h"

_Static_assert(CONFIG_HEADS == WS2811_CHIPS, "one head per chip on the chain");
_Static_assert(5 * CONFIG_SERVOS + 2 * CONFIG_TRACKS + CONFIG_ASPECTS * CONFIG_HEADS + 2 +
               6 * CONFIG_HEADS <= LCC_MAX_EVENTS,
               "every event needs a place in the LCC library's registry");

// Flashing counts ticks of a twelfth of a second. A rate is lit for a
// number of them, then dark for as many: 6 for 60 flashes a minute, 9 for
// 40 and 4 for 90. Every rate's cycle fits 72 ticks evenly.
#define FLASH_TICK_MS  83
#define FLASH_CYCLE    72
static const uint8_t flash_half[] = {
    [CONFIG_FLASH_MEDIUM] = 6,
    [CONFIG_FLASH_SLOW]   = 9,
    [CONFIG_FLASH_FAST]   = 4,
};

// Incandescent fade. A lamp's heat runs from 0 (cold) to HEAT_MAX (full),
// in a straight line over WARM_MS lighting or COOL_MS going out, and its
// output is brightness * heat², which the eye sees as a fairly even ramp.
#define FADE_TICK_MS  10
#define WARM_MS       150
#define COOL_MS       300
#define HEAT_BITS     10
#define HEAT_MAX      (1u << HEAT_BITS)
#define WARM_STEP     (HEAT_MAX * FADE_TICK_MS / WARM_MS)
#define COOL_STEP     (HEAT_MAX * FADE_TICK_MS / COOL_MS)

#define TEST_STEP_MS     1000
#define TEST_BRIGHTNESS  128

// The output each test step lights: red, yellow and green on SSSB wiring
// (outputs R, Y and G), then none
static const int8_t test_outputs[] = { 0, 2, 1, -1 };
#define TEST_STEPS ((int)sizeof(test_outputs))

static const struct {
    uint8_t lamp;
    bool flashing;
} aspects[CONFIG_ASPECTS] = {
    [CONFIG_ASPECT_DARK]            = { CONFIG_LAMP_NONE, false },
    [CONFIG_ASPECT_RED]             = { CONFIG_LAMP_RED, false },
    [CONFIG_ASPECT_YELLOW]          = { CONFIG_LAMP_YELLOW, false },
    [CONFIG_ASPECT_GREEN]           = { CONFIG_LAMP_GREEN, false },
    [CONFIG_ASPECT_LUNAR]           = { CONFIG_LAMP_LUNAR, false },
    [CONFIG_ASPECT_S]               = { CONFIG_LAMP_S, false },
    [CONFIG_ASPECT_D]               = { CONFIG_LAMP_D, false },
    [CONFIG_ASPECT_NUMBER]          = { CONFIG_LAMP_NUMBER, false },
    [CONFIG_ASPECT_FLASHING_RED]    = { CONFIG_LAMP_RED, true },
    [CONFIG_ASPECT_FLASHING_YELLOW] = { CONFIG_LAMP_YELLOW, true },
    [CONFIG_ASPECT_FLASHING_GREEN]  = { CONFIG_LAMP_GREEN, true },
};

static uint8_t aspect[CONFIG_HEADS];  // always dark while disabled
static bool enabled[CONFIG_HEADS];    // as last applied
static uint16_t heat[CONFIG_HEADS][3];
static uint8_t shown[WS2811_CHIPS][3];  // what the heads last gave the chain

// Runs while any head is flashing. Heads count the same ticks, so heads at
// one rate flash together.
static TimerHandle_t flash_timer;
static uint8_t flash_count;

// Outputs set to their own events: on or off. Off on any other output and
// while the head is disabled.
static bool lamp_on[CONFIG_HEADS][3];

// Runs while any lamp is warming or cooling
static TimerHandle_t fade_timer;

static TimerHandle_t test_timer;
static bool testing;
static int test_step;

static lcc_event_t events[CONFIG_HEADS][CONFIG_ASPECTS];
static lcc_event_t lamp_events[CONFIG_HEADS][3][2];  // on, off
static lcc_event_t test_events[2];  // start, stop
static const uint8_t no_event[8];  // the library skips all-zero IDs

static const config_head_t *head_cfg(int h)
{
    return &config.heads[h];
}

static int chip_of(int h)
{
    unsigned pos = head_cfg(h)->chain_position;
    return pos >= 1 && pos <= WS2811_CHIPS ? (int)pos - 1 : h;
}

// Whether a lamp flashing at the head's rate in this phase is lit now
static bool flash_lit(int h, int out)
{
    uint8_t rate = head_cfg(h)->flash_rate;
    unsigned half = flash_half[rate < sizeof(flash_half) ? rate : CONFIG_FLASH_MEDIUM];
    bool phase_a = (flash_count / half) % 2 == 0;
    return phase_a != (head_cfg(h)->output_phase[out] == CONFIG_PHASE_B);
}

// Whether a head's output should be lit right now. Every output with the
// aspect's lamp lights, so a lamp can take more than one.
static bool lit(int h, int out)
{
    uint8_t output = head_cfg(h)->output_lamp[out];
    if (output == CONFIG_LAMP_OWN)
        return lamp_on[h][out];
    uint8_t lamp = aspects[aspect[h]].lamp;
    return lamp != CONFIG_LAMP_NONE && output == lamp &&
           (!aspects[aspect[h]].flashing || flash_lit(h, out));
}

// Bring each lamp's heat toward lit or dark: at once on a head without fade,
// by a step each fade tick on one with it. Returns whether any lamp is
// still on its way.
static bool advance(bool tick)
{
    bool fading = false;
    for (int h = 0; h < CONFIG_HEADS; h++) {
        bool fades = head_cfg(h)->lamp_fade == CONFIG_FADE_INCANDESCENT;
        for (int i = 0; i < 3; i++) {
            uint16_t want = lit(h, i) ? HEAT_MAX : 0;
            uint16_t *t = &heat[h][i];
            if (!fades)
                *t = want;
            else if (tick && *t < want)
                *t = want - *t > WARM_STEP ? *t + WARM_STEP : want;
            else if (tick && *t > want)
                *t = *t > COOL_STEP ? *t - COOL_STEP : 0;
            fading |= *t != want;
        }
    }
    return fading;
}

static uint8_t level(int h, int out)
{
    uint32_t t = heat[h][out];
    return (uint8_t)(head_cfg(h)->output_brightness[out] * t * t >> (2 * HEAT_BITS));
}

// Give the chain what the heads show now: every chip, or only those that
// changed, so a chip set with the USB lights command keeps that until its
// head next changes. Nothing reaches the chain during a test.
static void render(bool all)
{
    if (testing)
        return;

    uint8_t out[WS2811_CHIPS][3] = {{0}};
    for (int h = 0; h < CONFIG_HEADS; h++) {
        int c = chip_of(h);
        for (int i = 0; i < 3; i++) {
            uint8_t v = level(h, i);
            if (v > out[c][i])
                out[c][i] = v;
        }
    }

    bool changed = false;
    for (int c = 0; c < WS2811_CHIPS; c++) {
        if (all || memcmp(out[c], shown[c], 3) != 0) {
            memcpy(shown[c], out[c], 3);
            ws2811_set(c, out[c][0], out[c][1], out[c][2]);
            changed = true;
        }
    }
    if (changed)
        ws2811_show();
}

// After anything that changes what a head should show
static void update(bool all)
{
    if (advance(false) && !xTimerIsTimerActive(fade_timer))
        xTimerStart(fade_timer, 0);
    render(all);
}

static void fade_tick(uint32_t arg)
{
    (void)arg;
    if (!advance(true))
        xTimerStop(fade_timer, 0);
    render(false);
}

static void fade_timer_cb(TimerHandle_t t)
{
    (void)t;
    lcc_task_call(fade_tick, 0);
}

static void update_flash_timer(void)
{
    bool flashing = false;
    for (int h = 0; h < CONFIG_HEADS; h++)
        flashing |= aspects[aspect[h]].flashing;

    if (!flashing) {
        xTimerStop(flash_timer, 0);
    } else if (!xTimerIsTimerActive(flash_timer)) {
        flash_count = 0;  // phase A lamps start lit
        xTimerStart(flash_timer, 0);
    }
}

static void flash_tick(uint32_t arg)
{
    (void)arg;
    flash_count = (flash_count + 1) % FLASH_CYCLE;
    update(false);
}

static void flash_timer_cb(TimerHandle_t t)
{
    (void)t;
    lcc_task_call(flash_tick, 0);
}

// ------------------------------------------------------------------
// Signal test
// ------------------------------------------------------------------

static void show_test_step(void)
{
    uint8_t v[3] = {0};
    if (test_outputs[test_step] >= 0)
        v[test_outputs[test_step]] = TEST_BRIGHTNESS;
    for (int c = 0; c < WS2811_CHIPS; c++)
        ws2811_set(c, v[0], v[1], v[2]);
    ws2811_show();
}

static void test_tick(uint32_t arg)
{
    (void)arg;
    if (!testing)
        return;  // queued before the test ended
    test_step = (test_step + 1) % TEST_STEPS;
    show_test_step();
}

static void test_timer_cb(TimerHandle_t t)
{
    (void)t;
    lcc_task_call(test_tick, 0);
}

void signals_test(bool on)
{
    if (on == testing)
        return;
    testing = on;
    if (on) {
        DBG("signal test\n");
        test_step = 0;
        show_test_step();
        xTimerStart(test_timer, 0);
    } else {
        DBG("signal test over\n");
        xTimerStop(test_timer, 0);
        render(true);
    }
}

bool signals_testing(void)
{
    return testing;
}

// A test event's argument is whether it starts the test
static void on_test_event(uintptr_t start)
{
    signals_test(start != 0);
}

static lcc_event_state_t test_state(uintptr_t start)
{
    return testing == (start != 0) ? LCC_EVENT_VALID : LCC_EVENT_INVALID;
}

// ------------------------------------------------------------------
// LCC events
// ------------------------------------------------------------------

// An event's argument is its head and aspect
static uintptr_t event_arg(int h, int a) { return (uintptr_t)(h * CONFIG_ASPECTS + a); }
static int arg_head(uintptr_t arg) { return (int)(arg / CONFIG_ASPECTS); }
static int arg_aspect(uintptr_t arg) { return (int)(arg % CONFIG_ASPECTS); }

static void on_aspect(uintptr_t arg)
{
    int h = arg_head(arg);
    DBG("signal head %d: aspect %d\n", h + 1, arg_aspect(arg));
    aspect[h] = (uint8_t)arg_aspect(arg);
    update_flash_timer();
    update(false);
}

static lcc_event_state_t aspect_state(uintptr_t arg)
{
    return aspect[arg_head(arg)] == arg_aspect(arg) ? LCC_EVENT_VALID : LCC_EVENT_INVALID;
}

// An own event's argument is its head, output and whether it's the On event
static uintptr_t lamp_arg(int h, int out, bool on) { return (uintptr_t)((h * 3 + out) * 2 + on); }
static int lamp_head(uintptr_t arg) { return (int)(arg / 6); }
static int lamp_output(uintptr_t arg) { return (int)(arg / 2 % 3); }
static bool lamp_is_on(uintptr_t arg) { return arg % 2 != 0; }

static void on_lamp(uintptr_t arg)
{
    static const char *const names[] = { "R", "G", "Y" };
    int h = lamp_head(arg), out = lamp_output(arg);
    DBG("signal head %d: output %s %s\n", h + 1, names[out], lamp_is_on(arg) ? "on" : "off");
    lamp_on[h][out] = lamp_is_on(arg);
    update(false);
}

static lcc_event_state_t lamp_state(uintptr_t arg)
{
    return lamp_on[lamp_head(arg)][lamp_output(arg)] == lamp_is_on(arg) ? LCC_EVENT_VALID
                                                                        : LCC_EVENT_INVALID;
}

// ------------------------------------------------------------------
// Configuration
// ------------------------------------------------------------------

// Dark, on a head with any lamp the aspects light
static bool can_show(const config_head_t *c, int a)
{
    for (int i = 0; i < 3; i++) {
        uint8_t lamp = c->output_lamp[i];
        if (a == CONFIG_ASPECT_DARK ? lamp != CONFIG_LAMP_NONE && lamp != CONFIG_LAMP_OWN
                                    : lamp == aspects[a].lamp)
            return true;
    }
    return false;
}

// Bring the heads into line with the configuration. A head being enabled
// starts on its power-on aspect. Only aspects a head can show are on the bus.
static void apply_config(void)
{
    ws2811_set_bit_rate(config.chain_speed == CONFIG_CHAIN_400KHZ ? WS2811_400KHZ : WS2811_800KHZ);
    ws2811_set_connector(config.chain_connector);

    for (int h = 0; h < CONFIG_HEADS; h++) {
        const config_head_t *c = head_cfg(h);
        bool on = c->enabled != 0;
        if (on != enabled[h]) {
            enabled[h] = on;
            aspect[h] = !on ? CONFIG_ASPECT_DARK
                      : c->power_on_aspect < CONFIG_ASPECTS ? c->power_on_aspect : CONFIG_ASPECT_RED;
        }
        for (int a = 0; a < CONFIG_ASPECTS; a++)
            events[h][a].id = on && can_show(c, a) ? c->aspect_events[a] : no_event;
        for (int i = 0; i < 3; i++) {
            bool own = on && c->output_lamp[i] == CONFIG_LAMP_OWN;
            if (!own)
                lamp_on[h][i] = false;  // and dark when it next becomes one
            lamp_events[h][i][0].id = own ? config.head_lamps[h].outputs[i].on_event : no_event;
            lamp_events[h][i][1].id = own ? config.head_lamps[h].outputs[i].off_event : no_event;
        }
    }
    test_events[0].id = config.test_start_event;
    test_events[1].id = config.test_stop_event;

    // Every chip, so a new chain speed or connector gets a frame at once
    update_flash_timer();
    update(true);
}

// Default event IDs: the node ID followed by a 16-bit index, with room for
// sixteen aspects per head, then the test's start and stop, then eight own
// events per head
static uint64_t default_event(uint64_t node_id, int h, int a)
{
    return node_id << 16 | (uint64_t)(0x100 + 16 * h + a);
}

static uint64_t default_test_event(uint64_t node_id, bool start)
{
    return node_id << 16 | (start ? 0x200 : 0x201);
}

static uint64_t default_lamp_event(uint64_t node_id, int h, int out, bool on)
{
    return node_id << 16 | (uint64_t)(0x300 + 8 * h + 2 * out + !on);
}

void signals_defaults(config_layout_t *cfg, uint64_t node_id)
{
    cfg->chain_speed = CONFIG_CHAIN_800KHZ;
    cfg->chain_connector = CONFIG_CONNECTOR_LED;
    for (int h = 0; h < CONFIG_HEADS; h++) {
        config_head_t *c = &cfg->heads[h];
        memset(c, 0, sizeof(*c));
        for (int a = 0; a < CONFIG_ASPECTS; a++)
            lcc_put_be64(c->aspect_events[a], default_event(node_id, h, a));
        c->power_on_aspect = CONFIG_ASPECT_RED;
        c->output_lamp[0] = CONFIG_LAMP_RED;  // SSSB wiring
        c->output_lamp[1] = CONFIG_LAMP_GREEN;
        c->output_lamp[2] = CONFIG_LAMP_YELLOW;
        memset(c->output_brightness, 128, sizeof(c->output_brightness));
        for (int i = 0; i < 3; i++) {
            lcc_put_be64(cfg->head_lamps[h].outputs[i].on_event, default_lamp_event(node_id, h, i, true));
            lcc_put_be64(cfg->head_lamps[h].outputs[i].off_event, default_lamp_event(node_id, h, i, false));
        }
    }
    lcc_put_be64(cfg->test_start_event, default_test_event(node_id, true));
    lcc_put_be64(cfg->test_stop_event, default_test_event(node_id, false));
}

static void migrate_event(uint8_t *field, uint64_t old_default, uint64_t new_default)
{
    if (lcc_get_be64(field) == old_default)
        lcc_put_be64(field, new_default);
}

void signals_migrate_events(config_layout_t *cfg, uint64_t old_id, uint64_t new_id)
{
    for (int h = 0; h < CONFIG_HEADS; h++) {
        for (int a = 0; a < CONFIG_ASPECTS; a++)
            migrate_event(cfg->heads[h].aspect_events[a], default_event(old_id, h, a),
                          default_event(new_id, h, a));
        for (int i = 0; i < 3; i++) {
            migrate_event(cfg->head_lamps[h].outputs[i].on_event, default_lamp_event(old_id, h, i, true),
                          default_lamp_event(new_id, h, i, true));
            migrate_event(cfg->head_lamps[h].outputs[i].off_event, default_lamp_event(old_id, h, i, false),
                          default_lamp_event(new_id, h, i, false));
        }
    }
    migrate_event(cfg->test_start_event, default_test_event(old_id, true),
                  default_test_event(new_id, true));
    migrate_event(cfg->test_stop_event, default_test_event(old_id, false),
                  default_test_event(new_id, false));
}

void signals_config_written(void)
{
    apply_config();
}

void signals_init(void)
{
    flash_timer = xTimerCreate("flash", pdMS_TO_TICKS(FLASH_TICK_MS), pdTRUE, NULL, flash_timer_cb);
    fade_timer = xTimerCreate("fade", pdMS_TO_TICKS(FADE_TICK_MS), pdTRUE, NULL, fade_timer_cb);
    test_timer = xTimerCreate("test", pdMS_TO_TICKS(TEST_STEP_MS), pdTRUE, NULL, test_timer_cb);

    for (int h = 0; h < CONFIG_HEADS; h++) {
        for (int a = 0; a < CONFIG_ASPECTS; a++) {
            events[h][a] = (lcc_event_t){ .id = no_event, .on_event = on_aspect,
                                          .state = aspect_state, .arg = event_arg(h, a) };
            lcc_event_register(&events[h][a]);
        }
        for (int i = 0; i < 3; i++) {
            for (int on = 1; on >= 0; on--) {
                lcc_event_t *e = &lamp_events[h][i][!on];
                *e = (lcc_event_t){ .id = no_event, .on_event = on_lamp, .state = lamp_state,
                                    .arg = lamp_arg(h, i, on) };
                lcc_event_register(e);
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        test_events[i] = (lcc_event_t){ .id = no_event, .on_event = on_test_event,
                                        .state = test_state, .arg = i == 0 };
        lcc_event_register(&test_events[i]);
    }
    apply_config();
}
