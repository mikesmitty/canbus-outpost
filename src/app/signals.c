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
_Static_assert(5 * CONFIG_SERVOS + 2 * CONFIG_TRACKS + CONFIG_ASPECTS * CONFIG_HEADS <= LCC_MAX_EVENTS,
               "every event needs a place in the LCC library's registry");

// Half a second on, half a second off: 60 flashes a minute
#define FLASH_MS 500

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

// Runs while any head is flashing; flashing heads all flash together
static TimerHandle_t flash_timer;
static bool flash_lit;

static lcc_event_t events[CONFIG_HEADS][CONFIG_ASPECTS];
static const uint8_t no_event[8];  // the library skips all-zero IDs

static const config_head_t *head_cfg(int h)
{
    return &config.heads[h];
}

// Set the chip to what the head should show right now. Every output with
// the aspect's lamp lights, so a lamp can take more than one.
static void show_head(int h)
{
    const config_head_t *c = head_cfg(h);
    uint8_t lamp = aspects[aspect[h]].lamp;
    bool lit = lamp != CONFIG_LAMP_NONE && (!aspects[aspect[h]].flashing || flash_lit);
    uint8_t out[3];

    for (int i = 0; i < 3; i++)
        out[i] = lit && c->output_lamp[i] == lamp ? c->output_brightness[i] : 0;
    ws2811_set(h, out[0], out[1], out[2]);
}

static void update_flash_timer(void)
{
    bool flashing = false;
    for (int h = 0; h < CONFIG_HEADS; h++)
        flashing |= aspects[aspect[h]].flashing;

    if (!flashing) {
        xTimerStop(flash_timer, 0);
    } else if (!xTimerIsTimerActive(flash_timer)) {
        flash_lit = true;
        xTimerStart(flash_timer, 0);
    }
}

static void flash_tick(uint32_t arg)
{
    (void)arg;
    flash_lit = !flash_lit;
    for (int h = 0; h < CONFIG_HEADS; h++) {
        if (aspects[aspect[h]].flashing)
            show_head(h);
    }
    ws2811_show();
}

static void flash_timer_cb(TimerHandle_t t)
{
    (void)t;
    lcc_task_call(flash_tick, 0);
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
    show_head(h);
    ws2811_show();
}

static lcc_event_state_t aspect_state(uintptr_t arg)
{
    return aspect[arg_head(arg)] == arg_aspect(arg) ? LCC_EVENT_VALID : LCC_EVENT_INVALID;
}

// ------------------------------------------------------------------
// Configuration
// ------------------------------------------------------------------

static bool can_show(const config_head_t *c, int a)
{
    if (a == CONFIG_ASPECT_DARK)
        return true;
    for (int i = 0; i < 3; i++) {
        if (c->output_lamp[i] == aspects[a].lamp)
            return true;
    }
    return false;
}

// Bring the heads into line with the configuration. A head being enabled
// starts on its power-on aspect. Only aspects a head can show are on the bus.
static void apply_config(void)
{
    ws2811_set_bit_rate(config.chain_speed == CONFIG_CHAIN_400KHZ ? WS2811_400KHZ : WS2811_800KHZ);

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
    }

    update_flash_timer();
    for (int h = 0; h < CONFIG_HEADS; h++)
        show_head(h);
    ws2811_show();
}

// Default event IDs: the node ID followed by a 16-bit index, with room for
// sixteen aspects per head
static uint64_t default_event(uint64_t node_id, int h, int a)
{
    return node_id << 16 | (uint64_t)(0x100 + 16 * h + a);
}

void signals_defaults(config_layout_t *cfg, uint64_t node_id)
{
    cfg->chain_speed = CONFIG_CHAIN_800KHZ;
    for (int h = 0; h < CONFIG_HEADS; h++) {
        config_head_t *c = &cfg->heads[h];
        memset(c, 0, sizeof(*c));
        for (int a = 0; a < CONFIG_ASPECTS; a++)
            lcc_put_be64(c->aspect_events[a], default_event(node_id, h, a));
        c->power_on_aspect = CONFIG_ASPECT_RED;
        c->output_lamp[0] = CONFIG_LAMP_RED;  // SSSB: yellow takes the blue output
        c->output_lamp[1] = CONFIG_LAMP_GREEN;
        c->output_lamp[2] = CONFIG_LAMP_YELLOW;
        memset(c->output_brightness, 128, sizeof(c->output_brightness));
    }
}

void signals_migrate_events(config_layout_t *cfg, uint64_t old_id, uint64_t new_id)
{
    for (int h = 0; h < CONFIG_HEADS; h++) {
        for (int a = 0; a < CONFIG_ASPECTS; a++) {
            uint8_t *field = cfg->heads[h].aspect_events[a];
            if (lcc_get_be64(field) == default_event(old_id, h, a))
                lcc_put_be64(field, default_event(new_id, h, a));
        }
    }
}

void signals_config_written(void)
{
    apply_config();
}

void signals_init(void)
{
    flash_timer = xTimerCreate("flash", pdMS_TO_TICKS(FLASH_MS), pdTRUE, NULL, flash_timer_cb);

    for (int h = 0; h < CONFIG_HEADS; h++) {
        for (int a = 0; a < CONFIG_ASPECTS; a++) {
            events[h][a] = (lcc_event_t){ .id = no_event, .on_event = on_aspect,
                                          .state = aspect_state, .arg = event_arg(h, a) };
            lcc_event_register(&events[h][a]);
        }
    }
    apply_config();
}
