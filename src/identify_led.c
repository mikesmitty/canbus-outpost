#include "identify_led.h"

#include <stdbool.h>

#include "FreeRTOS.h"
#include "board.h"
#include "pico/stdlib.h"
#include "task.h"

#define IDENTIFY_LED_BLINK_MS 250

static bool identify_active;
static bool identify_initialized;
static TickType_t identify_deadline;
static lcc_identify_config_t identify_config;

static TickType_t identify_timeout_ticks(uint8_t timeout_minutes) {
  if (timeout_minutes == LCC_IDENTIFY_TIMEOUT_INDEFINITE) return 0;

  return pdMS_TO_TICKS((uint32_t)timeout_minutes * 60u * 1000u);
}

static void identify_led_set(bool on) {
  gpio_put(IDENTIFY_LED_PIN, on ? 1 : 0);
}

static void identify_led_stop(void) {
  identify_active = false;
  identify_led_set(false);
}

void identify_led_init(const lcc_identify_config_t* config) {
  gpio_init(IDENTIFY_LED_PIN);
  gpio_set_dir(IDENTIFY_LED_PIN, GPIO_OUT);
  identify_initialized = true;
  identify_active = false;
  identify_led_set(false);
  identify_led_update_config(config);
}

void identify_led_update_config(const lcc_identify_config_t* config) {
  identify_config = *config;

  if (!identify_initialized) return;

  if (!identify_config.enabled) {
    identify_led_stop();
    return;
  }

  /* If enabled, ensure we are active and update/set the deadline */
  identify_active = true;
  TickType_t timeout = identify_timeout_ticks(identify_config.timeout_minutes);
  if (timeout == 0)
    identify_deadline = 0;
  else
    identify_deadline = xTaskGetTickCount() + timeout;
}

void identify_led_trigger(const lcc_identify_config_t* config) {
  identify_config = *config;
  if (!identify_initialized || !identify_config.enabled) return;

  identify_active = true;

  TickType_t timeout = identify_timeout_ticks(identify_config.timeout_minutes);
  if (timeout == 0)
    identify_deadline = 0;
  else
    identify_deadline = xTaskGetTickCount() + timeout;
}

bool identify_led_poll(void) {
  if (!identify_initialized || !identify_active) {
    if (identify_initialized) identify_led_set(false);
    return false;
  }

  if (identify_deadline != 0 && xTaskGetTickCount() >= identify_deadline) {
    identify_led_stop();
    return true;
  }

  TickType_t period = pdMS_TO_TICKS(IDENTIFY_LED_BLINK_MS);
  bool on = ((xTaskGetTickCount() / period) & 1u) == 0;
  identify_led_set(on);
  return false;
}
