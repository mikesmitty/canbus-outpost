#include "servo.h"
#include "board.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "util/dbg.h"

#define TICK_MS  20  /* servo_tick() called every 20ms */

static const uint8_t servo_pins[SERVO_COUNT] = {SERVO1_PIN, SERVO2_PIN, SERVO3_PIN, SERVO4_PIN};

/* Per-servo state */
static struct {
    uint16_t current;       /* Current position (0-65535) */
    uint16_t target;        /* Target position */
    uint16_t step;          /* Position increment per tick */
    uint16_t hold_ticks;    /* Ticks remaining before cutting PWM */
    bool     moving;        /* True while interpolating toward target */
    bool     holding;       /* True while in post-movement hold period */
    bool     active;        /* True if PWM is outputting a signal */
} servo_state[SERVO_COUNT];

static void servo_apply_position(int index, uint16_t value) {
    /* Map 0-65535 to 1000-2000 (1ms to 2ms) */
    uint32_t pulse = 1000 + ((uint32_t)value * 1000 / 65535);
    pwm_set_gpio_level(servo_pins[index], pulse);
}

void servo_init(void) {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    float div = (float)sys_hz / 1000000.0f;
    DBG("servo_init: clk_sys=%lu Hz, pwm div=%.1f\n", (unsigned long)sys_hz, (double)div);

    for (int i = 0; i < SERVO_COUNT; i++) {
        gpio_set_function(servo_pins[i], GPIO_FUNC_PWM);
        uint slice_num = pwm_gpio_to_slice_num(servo_pins[i]);
        uint channel = pwm_gpio_to_channel(servo_pins[i]);

        pwm_set_clkdiv(slice_num, div);
        pwm_set_wrap(slice_num, 20000);
        pwm_set_gpio_level(servo_pins[i], 0); /* 0% duty — no pulse, no power draw */
        pwm_set_enabled(slice_num, true);

        servo_state[i].current = 0;
        servo_state[i].target = 0;
        servo_state[i].step = 0;
        servo_state[i].hold_ticks = 0;
        servo_state[i].moving = false;
        servo_state[i].holding = false;
        servo_state[i].active = false;

        DBG("servo_init: ch%d gpio=%d slice=%d chan=%c\n",
            i, servo_pins[i], slice_num, 'A' + channel);
    }
}

void servo_disable(int index) {
    if (index < 0 || index >= SERVO_COUNT) return;
    pwm_set_gpio_level(servo_pins[index], 0); /* 0% duty — no signal */
    servo_state[index].moving = false;
    servo_state[index].holding = false;
    servo_state[index].active = false;
}

void servo_set_position(int index, uint16_t value) {
    if (index < 0 || index >= SERVO_COUNT) return;
    servo_state[index].current = value;
    servo_state[index].target = value;
    servo_state[index].moving = false;
    servo_state[index].holding = false;
    servo_state[index].active = true;
    servo_apply_position(index, value);
    DBG("servo_set_position: ch%d val=%u\n", index, value);
}

void servo_set_pulse_us(int index, uint16_t us) {
    if (index < 0 || index >= SERVO_COUNT) return;
    if (us < 500) us = 500;
    if (us > 2500) us = 2500;
    pwm_set_gpio_level(servo_pins[index], us);
    servo_state[index].active = true;
}

void servo_set_target(int index, uint16_t target, uint8_t throw_time, uint8_t hold_time) {
    if (index < 0 || index >= SERVO_COUNT) return;

    servo_state[index].target = target;
    servo_state[index].active = true;

    /* Calculate hold ticks: hold_time is in seconds, tick is 20ms */
    if (hold_time > 0)
        servo_state[index].hold_ticks = (uint16_t)hold_time * (1000 / TICK_MS);
    else
        servo_state[index].hold_ticks = 0; /* 0 = hold forever */
    servo_state[index].holding = false;

    if (servo_state[index].current == target || throw_time == 0) {
        /* Instant move or already there */
        servo_state[index].current = target;
        servo_state[index].moving = false;
        servo_state[index].step = 0;
        servo_apply_position(index, target);
        return;
    }

    /* Calculate step size from throw time and distance.
     * throw_time is in tenths of seconds.
     * total_ticks = (throw_time * 100ms) / 20ms = throw_time * 5 */
    uint32_t total_ticks = (uint32_t)throw_time * 5;
    if (total_ticks == 0) total_ticks = 1;

    uint32_t distance = (servo_state[index].current > target)
                      ? servo_state[index].current - target
                      : target - servo_state[index].current;

    uint32_t step = distance / total_ticks;
    if (step == 0) step = 1;
    if (step > 65535) step = 65535;
    servo_state[index].step = (uint16_t)step;
    servo_state[index].moving = true;

    DBG("servo_set_target: ch%d target=%u throw=%u.%us step=%u hold=%us\n",
        index, target, throw_time / 10, throw_time % 10,
        servo_state[index].step, hold_time);
}

uint8_t servo_tick(void) {
    uint8_t completed = 0;

    for (int i = 0; i < SERVO_COUNT; i++) {
        /* Handle post-movement hold countdown */
        if (servo_state[i].holding) {
            if (servo_state[i].hold_ticks > 0) {
                servo_state[i].hold_ticks--;
                if (servo_state[i].hold_ticks == 0) {
                    /* Hold expired — cut PWM */
                    pwm_set_gpio_level(servo_pins[i], 0);
                    servo_state[i].holding = false;
                    servo_state[i].active = false;
                    DBG("servo_tick: ch%d hold expired, PWM off\n", i);
                }
            }
            continue;
        }

        if (!servo_state[i].moving)
            continue;

        uint16_t cur = servo_state[i].current;
        uint16_t tgt = servo_state[i].target;
        uint16_t step = servo_state[i].step;

        if (cur < tgt) {
            uint16_t diff = tgt - cur;
            cur = (diff <= step) ? tgt : cur + step;
        } else {
            uint16_t diff = cur - tgt;
            cur = (diff <= step) ? tgt : cur - step;
        }

        servo_state[i].current = cur;
        servo_apply_position(i, cur);

        if (cur == tgt) {
            servo_state[i].moving = false;
            completed |= (1 << i);

            /* Start hold countdown if configured */
            if (servo_state[i].hold_ticks > 0) {
                servo_state[i].holding = true;
            }
            /* hold_ticks == 0 means hold forever (PWM stays on) */
        }
    }

    return completed;
}

bool servo_is_moving(int index) {
    if (index < 0 || index >= SERVO_COUNT) return false;
    return servo_state[index].moving;
}
