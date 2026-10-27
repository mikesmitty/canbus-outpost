#include "app.h"
#include <stddef.h>
#include <string.h>
#include "FreeRTOS.h"
#include "timers.h"
#include "config.h"
#include "identify.h"
#include "inputs.h"
#include "lcc/node.h"
#include "lcc_task.h"
#include "node_id.h"
#include "railcom_app.h"
#include "turnouts.h"
#include "usb_gridconnect.h"
#include "util/dbg.h"

// Configuration writes are saved this long after the last one, so a
// configuration tool's run of writes is one flash save
#define SAVE_DELAY_MS 1000

static bool id_assigned;
static TimerHandle_t save_timer;
static bool save_pending;

static void save(void)
{
    xTimerStop(save_timer, 0);
    save_pending = false;
    config_save();
}

static void save_in_task(uint32_t arg)
{
    (void)arg;
    app_config_flush();
}

static void save_timer_cb(TimerHandle_t t)
{
    if (!lcc_task_call(save_in_task, 0))
        xTimerReset(t, 0);  // the LCC task's queue is full: try again later
}

static void defaults(config_layout_t *cfg, uint64_t node_id)
{
    memset(cfg, 0, sizeof(*cfg));
    identify_defaults(cfg);
    turnouts_defaults(cfg, node_id);
    railcom_app_defaults(cfg, node_id);
}

static bool overlaps(uint32_t addr, uint32_t len, size_t start, size_t size)
{
    return addr < start + size && start < addr + len;
}

void app_init(uint64_t node_id, bool node_id_assigned)
{
    id_assigned = node_id_assigned;

    size_t loaded = config_load();
    if (loaded < sizeof(config)) {
        static config_layout_t d;
        defaults(&d, node_id);
        memcpy((uint8_t *)&config + loaded, (uint8_t *)&d + loaded, sizeof(config) - loaded);
        DBG(loaded ? "config: new fields set to defaults\n" : "config: using defaults\n");
    }

    save_timer = xTimerCreate("config_save", pdMS_TO_TICKS(SAVE_DELAY_MS), pdFALSE, NULL,
                              save_timer_cb);
    identify_init();
    inputs_init(turnouts_input_pressed);
    turnouts_init();
    railcom_app_init();
}

void app_start(void)
{
    inputs_start();
}

void app_config_written(uint32_t address, uint32_t length)
{
    if (overlaps(address, length, offsetof(config_layout_t, identify_enabled), 2))
        identify_apply();
    if (overlaps(address, length, offsetof(config_layout_t, servos), sizeof(config.servos)))
        turnouts_config_written();
    if (overlaps(address, length, offsetof(config_layout_t, tracks), sizeof(config.tracks)))
        railcom_app_config_written();

    save_pending = true;
    xTimerReset(save_timer, 0);
}

void app_config_flush(void)
{
    if (save_pending)
        save();
}

void app_factory_reset(void)
{
    DBG("config: factory reset\n");
    defaults(&config, lcc_node_id());
    save();
}

void app_identify_requested(void)
{
    identify_requested();
}

void app_duplicate_node_id(void)
{
    char id[18];
    node_id_format(lcc_node_id(), id);
    DBG("LCC: another node is using node ID %s\n", id);
    usb_console_printf("WARNING: another node is using this node ID (%s); "
                       "give one of them a new ID with the nodeid command\n", id);
    identify_alert();
}

void app_show_node_id(void)
{
    char id[18];
    node_id_format(lcc_node_id(), id);
    usb_console_printf("nodeid %s%s\n", id,
                       id_assigned ? "" : " (temporary, locally allocated from the chip ID)");
}

void app_set_node_id(uint64_t node_id)
{
    char id[18];
    node_id_format(node_id, id);

    if (!node_id_store(node_id)) {
        usb_console_printf("nodeid: saving %s failed\n", id);
        return;
    }
    turnouts_migrate_events(&config, lcc_node_id(), node_id);
    railcom_app_migrate_events(&config, lcc_node_id(), node_id);
    save();

    char last[18];
    node_id_format(node_id + CONFIG_TRACKS, last);
    usb_console_printf("nodeid set to %s, rebooting\n"
                       "RailCom tracks report as the next %d IDs, up to %s, unless configured otherwise\n",
                       id, CONFIG_TRACKS, last);
    lcc_task_reboot();  // after 100 ms, so the reply goes out too
}
