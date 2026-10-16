#include "lcc_task.h"
#include <string.h>
#include "queue.h"
#include "task.h"
#include "timers.h"
#include "hardware/watchdog.h"
#include "app/app.h"
#include "app/cdi_data.h"
#include "app/config.h"
#include "app/version.h"
#include "can_bus.h"
#include "firmware_update.h"
#include "lcc/memconfig.h"
#include "lcc/node.h"
#include "usb_gridconnect.h"
#include "util/dbg.h"

#define QUEUE_LEN            64
#define WATCHDOG_TIMEOUT_MS  5000
#define HEARTBEAT_MS         1000

typedef enum { MSG_FRAME, MSG_CALL } msg_type_t;

typedef struct {
    msg_type_t type;
    union {
        lcc_frame_t frame;
        struct {
            lcc_task_fn_t fn;
            uint32_t arg;
        } call;
    };
} msg_t;

static QueueHandle_t queue;
static TimerHandle_t heartbeat;
static volatile uint32_t dropped;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// ------------------------------------------------------------------
// Library hooks
// ------------------------------------------------------------------

static bool send_frame(const lcc_frame_t *frame)
{
    bool ok = can_bus_send(frame);
    usb_gc_send(frame);  // so a USB-connected host sees our traffic too
    return ok;
}

static void initialized(void)
{
    DBG("LCC: initialized, alias 0x%03X\n", lcc_alias());
    firmware_update_node_up();
}

static void reboot(void)
{
    DBG("LCC: rebooting\n");
    firmware_update_reboot();
}

static void update_complete(void)
{
    config_save();
}

static lcc_memspace_t cdi_space = { .space = LCC_SPACE_CDI };
static lcc_memspace_t config_space = { .space = LCC_SPACE_CONFIG };

void lcc_task_init(uint64_t node_id)
{
    queue = xQueueCreate(QUEUE_LEN, sizeof(msg_t));

    lcc_node_config_t cfg = {
        .node_id = node_id,
        .manufacturer = FIRMWARE_MANUFACTURER,
        .model = FIRMWARE_MODEL,
        .hardware_version = FIRMWARE_HW_VERSION,
        .software_version = FIRMWARE_SW_VERSION,
        .user_name = (const char *)config.user_name,
        .user_description = (const char *)config.user_description,
        .hooks = {
            .send = send_frame,
            .initialized = initialized,
            .identify_requested = app_identify_requested,
            .config_update_complete = update_complete,
            .factory_reset = app_factory_reset,
            .reboot = reboot,
        },
    };
    lcc_init(&cfg);

    cdi_space.size = cdi_xml_len;
    cdi_space.data = (const uint8_t *)cdi_xml;
    lcc_memconfig_add_space(&cdi_space);

    config_space.size = sizeof(config);
    config_space.data = (const uint8_t *)&config;
    config_space.writable = (uint8_t *)&config;
    config_space.written = app_config_written;
    lcc_memconfig_add_space(&config_space);

    firmware_update_init();
}

// ------------------------------------------------------------------
// Task
// ------------------------------------------------------------------

static void heartbeat_in_task(uint32_t arg)
{
    (void)arg;
    watchdog_update();

    static uint32_t reported;
    uint32_t d = dropped;
    if (d != reported) {
        DBG("LCC: %lu queue messages dropped\n", (unsigned long)(d - reported));
        reported = d;
    }
}

void lcc_task_start_watchdog(void)
{
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
}

static void heartbeat_cb(TimerHandle_t t)
{
    (void)t;
    lcc_task_call(heartbeat_in_task, 0);
}

void lcc_task(void *param)
{
    (void)param;

    // Fed through the queue, so a stuck task or timer service resets the node
    lcc_task_start_watchdog();
    heartbeat = xTimerCreate("heartbeat", pdMS_TO_TICKS(HEARTBEAT_MS), pdTRUE, NULL,
                             heartbeat_cb);
    xTimerStart(heartbeat, 0);

    app_start();
    lcc_start(now_ms());

    for (;;) {
        uint32_t wait = lcc_ms_until_next_timer(now_ms());
        TickType_t ticks = wait == LCC_NO_TIMER ? portMAX_DELAY : pdMS_TO_TICKS(wait);

        msg_t m;
        if (xQueueReceive(queue, &m, ticks) == pdTRUE) {
            if (m.type == MSG_FRAME)
                lcc_frame_received(&m.frame, now_ms());
            else
                m.call.fn(m.call.arg);
        }
        lcc_run_timers(now_ms());
    }
}

// ------------------------------------------------------------------
// Posting
// ------------------------------------------------------------------

static bool post(const msg_t *m)
{
    if (xQueueSend(queue, m, 0) == pdTRUE)
        return true;
    dropped++;
    return false;
}

static bool post_from_isr(const msg_t *m, BaseType_t *woken)
{
    if (xQueueSendFromISR(queue, m, woken) == pdTRUE)
        return true;
    dropped++;
    return false;
}

bool lcc_task_post_frame(const lcc_frame_t *frame)
{
    msg_t m = { .type = MSG_FRAME, .frame = *frame };
    return post(&m);
}

bool lcc_task_post_frame_from_isr(const lcc_frame_t *frame, BaseType_t *woken)
{
    msg_t m = { .type = MSG_FRAME, .frame = *frame };
    return post_from_isr(&m, woken);
}

bool lcc_task_call(lcc_task_fn_t fn, uint32_t arg)
{
    msg_t m = { .type = MSG_CALL, .call = { fn, arg } };
    return post(&m);
}

bool lcc_task_call_from_isr(lcc_task_fn_t fn, uint32_t arg, BaseType_t *woken)
{
    msg_t m = { .type = MSG_CALL, .call = { fn, arg } };
    return post_from_isr(&m, woken);
}
