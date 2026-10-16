#include "turnouts.h"
#include <string.h>
#include "FreeRTOS.h"
#include "timers.h"
#include "config.h"
#include "lcc/bytes.h"
#include "lcc/events.h"
#include "lcc_task.h"
#include "servo.h"
#include "util/dbg.h"

enum {
    EV_THROW,
    EV_CLOSE,
    EV_CENTER,
    EV_THROWN_FEEDBACK,
    EV_CLOSED_FEEDBACK,
    EV_PER_SERVO,
};

static struct {
    bool closed;
    bool known;  // false until commanded, and after centering
} state[CONFIG_SERVOS];

static lcc_event_t events[CONFIG_SERVOS][EV_PER_SERVO];

// Runs servo_tick() while any servo is moving or holding
static TimerHandle_t motion_timer;

static const config_servo_t *servo_cfg(int i)
{
    return &config.servos[i];
}

static void send_feedback(int i)
{
    const config_servo_t *s = servo_cfg(i);
    lcc_event_send(state[i].closed ? s->closed_feedback : s->thrown_feedback);
}

static void motion_tick(uint32_t arg)
{
    (void)arg;
    uint8_t done = servo_tick();
    for (int i = 0; i < CONFIG_SERVOS; i++) {
        if (done & (1u << i)) {
            send_feedback(i);
            DBG("turnout %d: arrived %s\n", i + 1, state[i].closed ? "closed" : "thrown");
        }
    }
    if (!servo_busy())
        xTimerStop(motion_timer, 0);
}

static void motion_timer_cb(TimerHandle_t t)
{
    (void)t;
    lcc_task_call(motion_tick, 0);
}

static void move(int i, bool closed, uint8_t throw_time)
{
    const config_servo_t *s = servo_cfg(i);
    uint16_t thrown_pos = lcc_get_be16(s->thrown_position);
    uint16_t closed_pos = lcc_get_be16(s->closed_position);
    bool to_closed_pos = closed != (s->reversed != 0);

    servo_set_target(i, to_closed_pos ? closed_pos : thrown_pos, throw_time,
                     s->hold_time, s->frog_output,
                     (uint16_t)(((uint32_t)thrown_pos + closed_pos) / 2));
    state[i].closed = closed;
    state[i].known = true;

    if (!servo_is_moving(i))
        send_feedback(i);  // instant move, or already there
    if (servo_busy())
        xTimerStart(motion_timer, 0);
}

// ------------------------------------------------------------------
// LCC events
// ------------------------------------------------------------------

static void on_throw(uintptr_t i)
{
    if (servo_cfg(i)->enabled) {
        DBG("turnout %d: throw\n", (int)i + 1);
        move(i, false, servo_cfg(i)->throw_time);
    }
}

static void on_close(uintptr_t i)
{
    if (servo_cfg(i)->enabled) {
        DBG("turnout %d: close\n", (int)i + 1);
        move(i, true, servo_cfg(i)->throw_time);
    }
}

static void on_center(uintptr_t i)
{
    const config_servo_t *s = servo_cfg(i);
    if (!s->enabled)
        return;
    DBG("turnout %d: center\n", (int)i + 1);
    // Instant, PWM held, frog untouched: for setting up linkages
    servo_set_target(i, lcc_get_be16(s->center_position), 0, 0, 0, 0);
    state[i].known = false;
}

static lcc_event_state_t command_state(uintptr_t i, bool closed)
{
    if (!state[i].known)
        return LCC_EVENT_UNKNOWN;
    return state[i].closed == closed ? LCC_EVENT_VALID : LCC_EVENT_INVALID;
}

static lcc_event_state_t throw_state(uintptr_t i) { return command_state(i, false); }
static lcc_event_state_t close_state(uintptr_t i) { return command_state(i, true); }

static lcc_event_state_t feedback_state(uintptr_t i, bool closed)
{
    if (!state[i].known)
        return LCC_EVENT_UNKNOWN;
    return state[i].closed == closed && !servo_is_moving(i) ? LCC_EVENT_VALID
                                                            : LCC_EVENT_INVALID;
}

static lcc_event_state_t thrown_feedback_state(uintptr_t i) { return feedback_state(i, false); }
static lcc_event_state_t closed_feedback_state(uintptr_t i) { return feedback_state(i, true); }

static void register_events(int i)
{
    config_servo_t *s = &config.servos[i];
    lcc_event_t *ev = events[i];

    ev[EV_THROW] = (lcc_event_t){ .id = s->throw_event, .on_event = on_throw,
                                  .state = throw_state, .arg = i };
    ev[EV_CLOSE] = (lcc_event_t){ .id = s->close_event, .on_event = on_close,
                                  .state = close_state, .arg = i };
    ev[EV_CENTER] = (lcc_event_t){ .id = s->center_event, .on_event = on_center, .arg = i };
    ev[EV_THROWN_FEEDBACK] = (lcc_event_t){ .id = s->thrown_feedback, .producer = true,
                                            .state = thrown_feedback_state, .arg = i };
    ev[EV_CLOSED_FEEDBACK] = (lcc_event_t){ .id = s->closed_feedback, .producer = true,
                                            .state = closed_feedback_state, .arg = i };

    for (int k = 0; k < EV_PER_SERVO; k++)
        lcc_event_register(&ev[k]);
}

// ------------------------------------------------------------------
// Public
// ------------------------------------------------------------------

static uint8_t *event_field(config_servo_t *s, int k)
{
    switch (k) {
    case EV_THROW:           return s->throw_event;
    case EV_CLOSE:           return s->close_event;
    case EV_THROWN_FEEDBACK: return s->thrown_feedback;
    case EV_CLOSED_FEEDBACK: return s->closed_feedback;
    default:                 return s->center_event;
    }
}

// Default event IDs: node ID followed by a 16-bit index. The index order
// (throw, close, thrown feedback, closed feedback, center) predates the
// center event, which was added last.
static uint64_t default_event(uint64_t node_id, int servo, int k)
{
    static const uint8_t index[EV_PER_SERVO] = {
        [EV_THROW] = 0, [EV_CLOSE] = 1, [EV_THROWN_FEEDBACK] = 2,
        [EV_CLOSED_FEEDBACK] = 3, [EV_CENTER] = 4,
    };
    return (node_id << 16) | (uint64_t)(servo * EV_PER_SERVO + index[k]);
}

void turnouts_defaults(config_layout_t *cfg, uint64_t node_id)
{
    for (int i = 0; i < CONFIG_SERVOS; i++) {
        config_servo_t *s = &cfg->servos[i];
        memset(s, 0, sizeof(*s));
        for (int k = 0; k < EV_PER_SERVO; k++)
            lcc_put_be64(event_field(s, k), default_event(node_id, i, k));
        lcc_put_be16(s->thrown_position, 16384);
        lcc_put_be16(s->closed_position, 49152);
        lcc_put_be16(s->center_position, 32768);
        s->throw_time = 30;  // 3.0 s
        s->hold_time = 2;
        s->enabled = 1;
        s->default_state = CONFIG_DEFAULT_CLOSED;
    }
}

void turnouts_migrate_events(config_layout_t *cfg, uint64_t old_id, uint64_t new_id)
{
    for (int i = 0; i < CONFIG_SERVOS; i++) {
        for (int k = 0; k < EV_PER_SERVO; k++) {
            uint8_t *field = event_field(&cfg->servos[i], k);
            if (lcc_get_be64(field) == default_event(old_id, i, k))
                lcc_put_be64(field, default_event(new_id, i, k));
        }
    }
}

void turnouts_init(void)
{
    motion_timer = xTimerCreate("servo", pdMS_TO_TICKS(SERVO_TICK_MS), pdTRUE, NULL,
                                motion_timer_cb);

    for (int i = 0; i < CONFIG_SERVOS; i++) {
        register_events(i);

        const config_servo_t *s = servo_cfg(i);
        if (!s->enabled || s->default_state == CONFIG_DEFAULT_NO_CHANGE)
            continue;  // PWM stays off; position unknown until commanded
        move(i, s->default_state == CONFIG_DEFAULT_CLOSED, 0);
    }
}

void turnouts_config_written(void)
{
    for (int i = 0; i < CONFIG_SERVOS; i++) {
        if (!servo_cfg(i)->enabled) {
            servo_disable(i);
            state[i].known = false;
        }
    }
}

void turnouts_input_pressed(int input)
{
    for (int i = 0; i < CONFIG_SERVOS; i++) {
        const config_servo_t *s = servo_cfg(i);
        if (s->enabled && s->fascia_input == input) {
            DBG("turnout %d: fascia input %d\n", i + 1, input);
            move(i, !state[i].closed, s->throw_time);
        }
    }
}
