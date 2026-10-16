#include "identify.h"
#include "FreeRTOS.h"
#include "timers.h"
#include "board.h"
#include "config.h"
#include "lcc_task.h"
#include "util/dbg.h"

#define BLINK_MS 250
#define ALERT_BLINK_MS 80

static bool alert;  // fast blink until reboot, whatever the identify setting

static TimerHandle_t blink_timer;
static TimerHandle_t timeout_timer;
static bool led_on;

static void led(bool on)
{
    led_on = on;
    gpio_put(IDENTIFY_LED_PIN, on);
}

static void blink_cb(TimerHandle_t t)
{
    (void)t;
    led(!led_on);
}

static void stop(void)
{
    xTimerStop(timeout_timer, 0);
    if (alert)
        return;
    // The timer task outranks the LCC task, so no blink runs after this
    xTimerStop(blink_timer, 0);
    led(false);
}

static void expired(uint32_t arg)
{
    (void)arg;
    DBG("identify: timed out\n");
    config.identify_enabled = 0;
    config_save();
    stop();
}

static void timeout_cb(TimerHandle_t t)
{
    (void)t;
    lcc_task_call(expired, 0);
}

static void restart_timeout(void)
{
    uint8_t minutes = config.identify_timeout_minutes;
    if (minutes == 0)
        xTimerStop(timeout_timer, 0);
    else
        xTimerChangePeriod(timeout_timer, pdMS_TO_TICKS(minutes * 60u * 1000u), 0);
}

void identify_defaults(config_layout_t *cfg)
{
    cfg->identify_enabled = 0;
    cfg->identify_timeout_minutes = 5;
}

void identify_init(void)
{
    gpio_init(IDENTIFY_LED_PIN);
    gpio_set_dir(IDENTIFY_LED_PIN, GPIO_OUT);
    led(false);

    blink_timer = xTimerCreate("blink", pdMS_TO_TICKS(BLINK_MS), pdTRUE, NULL, blink_cb);
    timeout_timer = xTimerCreate("identify", pdMS_TO_TICKS(60u * 1000u), pdFALSE, NULL,
                                 timeout_cb);
    identify_apply();
}

void identify_apply(void)
{
    if (!config.identify_enabled) {
        stop();
        return;
    }
    if (!xTimerIsTimerActive(blink_timer))
        xTimerStart(blink_timer, 0);
    restart_timeout();
}

void identify_alert(void)
{
    alert = true;
    xTimerChangePeriod(blink_timer, pdMS_TO_TICKS(ALERT_BLINK_MS), 0);  // starts it too
}

void identify_requested(void)
{
    if (config.identify_enabled)
        restart_timeout();
}
