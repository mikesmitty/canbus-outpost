#include "firmware_update.h"
#include <string.h>
#include "FreeRTOS.h"
#include "timers.h"
#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "boot/uf2.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "fw_image.h"
#include "lcc/memconfig.h"
#include "lcc_task.h"
#include "util/dbg.h"

// A trial image confirms itself once it has been up this long
#define CONFIRM_DELAY_MS  5000
// and goes back to the previous image if it hasn't by this time
#define TRIAL_LIMIT_MS    60000

// Bootrom work area (partition lookups, confirming), word aligned
static __attribute__((aligned(4))) uint8_t workarea[4096];

static int running_partition = -1;
static bool on_trial;
static TimerHandle_t trial_timer;
static TimerHandle_t confirm_timer;

// The transfer: bytes go through sector_buf into the target partition
static uint8_t sector_buf[FLASH_SECTOR_SIZE];
static uint32_t target_offset;  // flash offset of the partition being written
static uint32_t received;       // image bytes accepted, contiguous from 0
static uint32_t flushed;        // bytes of the partition written so far
static uint32_t last_address, last_len;  // the last write accepted
static bool start_new;          // the image passed its checks at Unfreeze

// ------------------------------------------------------------------
// Flash
// ------------------------------------------------------------------

static void program_sector(void *param)
{
    uint32_t offset = *(const uint32_t *)param;
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, sector_buf, FLASH_SECTOR_SIZE);
}

// The partitions are outside the translated XIP window this image runs
// in, so read flash at its physical address.
static const uint8_t *flash_physical(uint32_t offset)
{
    return (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + offset);
}

// Write sector_buf as the next sector of the partition. Interrupts are off
// while the sector is erased and programmed (tens of milliseconds); the
// client waits for this write's acknowledgment, so nothing of its is lost.
static bool flush_sector(void)
{
    uint32_t offset = target_offset + flushed;
    int rc = flash_safe_execute(program_sector, &offset, 500);
    if (rc != PICO_OK) {
        DBG("firmware: flash write at 0x%06lx failed (%d)\n", (unsigned long)offset, rc);
        return false;
    }
    if (memcmp(flash_physical(offset), sector_buf, FLASH_SECTOR_SIZE) != 0) {
        DBG("firmware: flash at 0x%06lx doesn't read back\n", (unsigned long)offset);
        return false;
    }
    flushed += FLASH_SECTOR_SIZE;
    return true;
}

// ------------------------------------------------------------------
// LCC firmware upgrade hooks
// ------------------------------------------------------------------

static uint16_t freeze(void);
static uint16_t write_image(uint32_t address, const uint8_t *data, uint32_t len);
static uint16_t unfreeze(void);

static lcc_firmware_upgrade_t upgrade = {
    .freeze = freeze,
    .write = write_image,
    .unfreeze = unfreeze,
};

static uint16_t freeze(void)
{
    if (on_trial)
        return LCC_ERROR_TEMPORARY;  // the running image must confirm itself first

    // The partition that won't boot as things stand: the other one
    resident_partition_t p;
    rom_flash_flush_cache();
    int part = rom_get_uf2_target_partition(workarea, sizeof(workarea),
                                            RP2350_ARM_S_FAMILY_ID, &p);
    if (part < 0 || part == running_partition) {
        DBG("firmware: no partition to update (%d, running %d)\n", part, running_partition);
        return LCC_ERROR_PERMANENT;
    }
    uint32_t first = (p.permissions_and_location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                     PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (p.permissions_and_location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
                    PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    target_offset = first * FLASH_SECTOR_SIZE;
    upgrade.max_size = (last - first + 1) * FLASH_SECTOR_SIZE;
    received = flushed = 0;
    start_new = false;

    DBG("firmware: upgrade started, writing partition %c (0x%06lx, %lu KB)\n", 'A' + part,
        (unsigned long)target_offset, (unsigned long)(upgrade.max_size / 1024));
    return 0;
}

static uint16_t write_image(uint32_t address, const uint8_t *data, uint32_t len)
{
    if (received && address == last_address && len == last_len)
        return 0;  // the client resent the last write: its acknowledgment was lost
    if (address == 0) {
        received = flushed = 0;  // a new attempt
        if (!fw_image_start_ok(data, len)) {
            DBG("firmware: not an image for this board\n");
            return LCC_ERROR_FIRMWARE_INCOMPATIBLE;
        }
    }
    if (address != received || received < flushed)
        return LCC_ERROR_INVALID_ARGS;  // out of order, or after the end was written

    uint32_t fill = received - flushed;
    uint32_t n = len < FLASH_SECTOR_SIZE - fill ? len : FLASH_SECTOR_SIZE - fill;
    memcpy(sector_buf + fill, data, n);
    if (fill + n == FLASH_SECTOR_SIZE) {
        if (!flush_sector())
            return LCC_ERROR_FIRMWARE_CHECKSUM;  // the client resends this write
        memcpy(sector_buf, data + n, len - n);
    }
    received += len;
    last_address = address;
    last_len = len;
    return 0;
}

static uint16_t unfreeze(void)
{
    if (received > flushed) {
        uint32_t fill = received - flushed;
        memset(sector_buf + fill, 0xFF, FLASH_SECTOR_SIZE - fill);
        if (!flush_sector())
            return LCC_ERROR_FIRMWARE_CHECKSUM;
    }

    switch (fw_image_check(flash_physical(target_offset), received)) {
    case FW_IMAGE_OK:
        DBG("firmware: received a %lu byte image\n", (unsigned long)received);
        start_new = true;
        return 0;
    case FW_IMAGE_INCOMPLETE:
        DBG("firmware: image incomplete (%lu bytes received)\n", (unsigned long)received);
        return LCC_ERROR_FIRMWARE_CHECKSUM;
    case FW_IMAGE_INCOMPATIBLE:
        DBG("firmware: image is not for this board, or was built without a hash\n");
        return LCC_ERROR_FIRMWARE_INCOMPATIBLE;
    default:
        DBG("firmware: image metadata damaged\n");
        return LCC_ERROR_FIRMWARE_CORRUPTED;
    }
}

// ------------------------------------------------------------------
// Trial of a new image
// ------------------------------------------------------------------

static void trial_expired(TimerHandle_t t)
{
    (void)t;
    DBG("firmware: new image not confirmed, going back to the previous one\n");
    watchdog_reboot(0, 0, 0);  // a normal boot skips an unconfirmed image
}

static void confirm_in_task(uint32_t arg)
{
    (void)arg;
    int rc = rom_explicit_buy(workarea, sizeof(workarea));
    // The bootrom stops the watchdog while confirming
    lcc_task_start_watchdog();
    if (rc != 0) {
        DBG("firmware: confirming the new image failed (%d)\n", rc);
        return;  // the trial runs out and the previous image comes back
    }
    on_trial = false;
    xTimerStop(trial_timer, 0);
    DBG("firmware: new image confirmed\n");
}

static void confirm_cb(TimerHandle_t t)
{
    (void)t;
    lcc_task_call(confirm_in_task, 0);
}

// ------------------------------------------------------------------
// Public
// ------------------------------------------------------------------

void firmware_update_init(void)
{
    boot_info_t bi;
    if (!rom_get_boot_info(&bi)) {
        DBG("firmware: no boot information, LCC firmware updates off\n");
        return;
    }

    on_trial = (bi.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) != 0;
    if (on_trial) {
        DBG("firmware: new image on trial\n");
        trial_timer = xTimerCreate("fw_trial", pdMS_TO_TICKS(TRIAL_LIMIT_MS), pdFALSE, NULL,
                                   trial_expired);
        confirm_timer = xTimerCreate("fw_confirm", pdMS_TO_TICKS(CONFIRM_DELAY_MS), pdFALSE,
                                     NULL, confirm_cb);
        xTimerStart(trial_timer, 0);
    }

    if (bi.partition < 0) {
        DBG("firmware: no partition table, LCC firmware updates off "
            "(install one with make partition-table)\n");
        return;
    }
    running_partition = bi.partition;
    DBG("firmware: running from partition %c\n", 'A' + bi.partition);
    lcc_memconfig_enable_firmware_upgrade(&upgrade);
}

void firmware_update_node_up(void)
{
    if (on_trial && !xTimerIsTimerActive(confirm_timer))
        xTimerStart(confirm_timer, 0);
}

void firmware_update_reboot(void)
{
    if (start_new) {
        DBG("firmware: starting the new image\n");
        rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10,
                   XIP_BASE + target_offset, 0);
    }
    watchdog_reboot(0, 0, 0);
}
