#include "nv_storage.h"
#include <string.h>
#include "hardware/flash.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "boot/picobin.h"
#include "util/dbg.h"

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

_Static_assert(NV_SECTOR_SIZE == FLASH_SECTOR_SIZE, "flash sector size");

#define TOP(kb) (PICO_FLASH_SIZE_BYTES - (kb) * 1024)

const nv_region_t nv_config_region = {
    .flash_offset = TOP(64),
    .magic = 0x4F43464Eu,  // "NFCO"
    .slot_sectors = NV_CONFIG_SECTORS,
};

const nv_region_t nv_identity_region = {
    .flash_offset = TOP(16),
    .magic = 0x4449434Eu,  // "NCID"
    .slot_sectors = 1,
};

const nv_region_t nv_old_config_region = {
    .flash_offset = TOP(8),
    .magic = 0x4F43464Eu,
    .slot_sectors = 1,
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t sequence;
    uint32_t crc;  // over the header up to here and the data
} record_header_t;

_Static_assert(sizeof(record_header_t) == NV_HEADER_SIZE, "record header size");
_Static_assert(NV_MAX_DATA(NV_CONFIG_SECTORS) <= UINT16_MAX, "record length must fit its field");

static uint32_t slot_bytes(const nv_region_t *r)
{
    return r->slot_sectors * FLASH_SECTOR_SIZE;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

static uint32_t record_crc(const record_header_t *h, const uint8_t *data)
{
    uint32_t crc = crc32_update(0, (const uint8_t *)h, offsetof(record_header_t, crc));
    return crc32_update(crc, data, h->length);
}

// Read at the physical flash address: booted from a partition, the usual
// XIP window maps only that partition, and these records are outside it.
static const record_header_t *slot_header(const nv_region_t *r, int slot)
{
    return (const record_header_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + r->flash_offset +
                                     slot * slot_bytes(r));
}

static bool slot_valid(const nv_region_t *r, int slot)
{
    const record_header_t *h = slot_header(r, slot);
    if (h->magic != r->magic || h->length > NV_MAX_DATA(r->slot_sectors))
        return false;
    return record_crc(h, (const uint8_t *)(h + 1)) == h->crc;
}

// Slot holding the newest valid record, or -1
static int newest_slot(const nv_region_t *r)
{
    bool v0 = slot_valid(r, 0), v1 = slot_valid(r, 1);
    if (v0 && v1)
        return (int32_t)(slot_header(r, 1)->sequence - slot_header(r, 0)->sequence) > 0;
    return v0 ? 0 : v1 ? 1 : -1;
}

bool nv_load(const nv_region_t *region, void *buf, size_t size,
             size_t *stored_len, uint16_t *version)
{
    int slot = newest_slot(region);
    if (slot < 0)
        return false;

    const record_header_t *h = slot_header(region, slot);
    size_t n = h->length < size ? h->length : size;
    memcpy(buf, h + 1, n);
    if (stored_len)
        *stored_len = h->length;
    if (version)
        *version = h->version;
    return true;
}

// One sector at a time, also the boot ROM's work area for reading the
// partition table (which needs 3264 bytes)
static __attribute__((aligned(4))) uint8_t sector_buf[FLASH_SECTOR_SIZE];

static void write_sector(void *param)
{
    uint32_t offset = *(const uint32_t *)param;
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, sector_buf, FLASH_SECTOR_SIZE);
}

bool nv_save(const nv_region_t *region, const void *buf, size_t len,
             uint16_t version)
{
    if (len > NV_MAX_DATA(region->slot_sectors))
        return false;

    int newest = newest_slot(region);
    if (newest >= 0) {
        // Already saved: skip the erase and its time with interrupts off
        const record_header_t *n = slot_header(region, newest);
        if (n->version == version && n->length == len && memcmp(n + 1, buf, len) == 0)
            return true;
    }
    int slot = newest == 0 ? 1 : 0;
    uint32_t sequence = newest < 0 ? 1 : slot_header(region, newest)->sequence + 1;

    record_header_t h = {
        .magic = region->magic,
        .version = version,
        .length = (uint16_t)len,
        .sequence = sequence,
    };
    h.crc = record_crc(&h, buf);

    // The record as it lies in the slot: the header, then the data. The
    // header's sector goes last, so the slot can't hold a new header ahead
    // of the rest of its data. Interrupts come back on between sectors.
    uint32_t total = sizeof(h) + len;
    uint32_t sectors = (total + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
    for (int s = (int)sectors - 1; s >= 0; s--) {
        uint32_t start = s * FLASH_SECTOR_SIZE;
        uint32_t end = start + FLASH_SECTOR_SIZE < total ? start + FLASH_SECTOR_SIZE : total;
        memset(sector_buf, 0xFF, sizeof(sector_buf));
        for (uint32_t i = start; i < end; i++)
            sector_buf[i - start] = i < sizeof(h) ? ((const uint8_t *)&h)[i]
                                                  : ((const uint8_t *)buf)[i - sizeof(h)];
        uint32_t offset = region->flash_offset + slot * slot_bytes(region) + start;
        int rc = flash_safe_execute(write_sector, &offset, 500);
        if (rc != PICO_OK) {
            DBG("nv_save: flash_safe_execute failed (%d)\n", rc);
            return false;
        }
    }
    return slot_valid(region, slot);
}

bool nv_region_outside_partitions(const nv_region_t *region)
{
    // Partitions are counted in sectors
    uint32_t first = region->flash_offset / FLASH_SECTOR_SIZE;
    uint32_t last = first + 2 * region->slot_sectors - 1;

    // Loaded at boot, unless the boot skipped it
    int rc = rom_load_partition_table(sector_buf, sizeof(sector_buf), false);
    if (rc != BOOTROM_OK) {
        DBG("nv: partition table not loaded (%d)\n", rc);
        return false;
    }
    // The flags supported, three words about the table, then two per partition
    static uint32_t info[1 + 3 + 2 * PARTITION_TABLE_MAX_PARTITIONS];
    const uint32_t flags = PT_INFO_PT_INFO | PT_INFO_PARTITION_LOCATION_AND_FLAGS;
    int words = rom_get_partition_table_info(info, sizeof(info) / sizeof(info[0]), flags);
    if (words < 4 || info[0] != flags) {
        DBG("nv: partition table unreadable (%d)\n", words);
        return false;
    }
    int count = info[1] & 0xFF;
    for (int p = 0; p < count; p++) {
        int w = 4 + 2 * p;
        if (w >= words)
            return false;
        uint32_t loc = info[w];
        uint32_t p_first = (loc & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                           PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
        uint32_t p_last = (loc & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
                          PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
        if (p_first <= last && first <= p_last)
            return false;
    }
    return true;
}
