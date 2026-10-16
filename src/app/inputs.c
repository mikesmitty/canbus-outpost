#include "inputs.h"
#include "FreeRTOS.h"
#include "timers.h"
#include "board.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "lcc_task.h"

#define INPUT_COUNT  4
#define DEBOUNCE_MS  30
#define EDGES        (GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE)
#define LEVEL_LOW    0x100u

static const uint8_t pins[INPUT_COUNT] = { GPIO_IN1, GPIO_IN2, GPIO_IN3, GPIO_IN4 };
static TimerHandle_t debounce[INPUT_COUNT];
static bool pressed[INPUT_COUNT];
static void (*press_handler)(int input);

static void settled(uint32_t arg)
{
    int i = arg & 0xFF;
    bool now_pressed = (arg & LEVEL_LOW) != 0;
    if (now_pressed == pressed[i])
        return;
    pressed[i] = now_pressed;
    if (now_pressed && press_handler)
        press_handler(i + 1);
}

static void debounce_cb(TimerHandle_t t)
{
    int i = (int)(uintptr_t)pvTimerGetTimerID(t);
    // Re-arm before reading, so a change after this read starts a new cycle
    // (enabling also clears the edges seen while disabled)
    gpio_set_irq_enabled(pins[i], EDGES, true);
    uint32_t level = gpio_get(pins[i]) ? 0 : LEVEL_LOW;
    lcc_task_call(settled, (uint32_t)i | level);
}

// The first edge disables that pin's interrupt until the debounce timer
// reads the settled level, so contact bounce costs one timer command
// rather than flooding the timer queue.
static void edge_irq(void)
{
    BaseType_t woken = pdFALSE;
    for (int i = 0; i < INPUT_COUNT; i++) {
        uint32_t events = gpio_get_irq_event_mask(pins[i]) & EDGES;
        if (events) {
            gpio_set_irq_enabled(pins[i], EDGES, false);
            gpio_acknowledge_irq(pins[i], events);
            xTimerStartFromISR(debounce[i], &woken);
        }
    }
    portYIELD_FROM_ISR(woken);
}

void inputs_init(void (*on_press)(int input))
{
    press_handler = on_press;
    for (int i = 0; i < INPUT_COUNT; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
        pressed[i] = !gpio_get(pins[i]);
        debounce[i] = xTimerCreate("input", pdMS_TO_TICKS(DEBOUNCE_MS), pdFALSE,
                                   (void *)(uintptr_t)i, debounce_cb);
    }
}

void inputs_start(void)
{
    uint32_t mask = 0;
    for (int i = 0; i < INPUT_COUNT; i++)
        mask |= 1u << pins[i];
    gpio_add_raw_irq_handler_masked(mask, edge_irq);
    for (int i = 0; i < INPUT_COUNT; i++)
        gpio_set_irq_enabled(pins[i], EDGES, true);
    irq_set_enabled(IO_IRQ_BANK0, true);
}
