#include "nv_storage.h"
#include <string.h>
#include "hardware/flash.h"
#include "pico/flash.h"
#include "util/dbg.h"

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

#define REGION_BYTES (2 * FLASH_SECTOR_SIZE)

const nv_region_t nv_config_region = {
    .flash_offset = PICO_FLASH_SIZE_BYTES - 1 * REGION_BYTES,
    .magic = 0x4F43464Eu,  // "NFCO"
};

const nv_region_t nv_identity_region = {
    .flash_offset = PICO_FLASH_SIZE_BYTES - 2 * REGION_BYTES,
    .magic = 0x4449434Eu,  // "NCID"
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t sequence;
    uint32_t crc;  // over the header up to here and the data
} record_header_t;

#define MAX_DATA (FLASH_SECTOR_SIZE - sizeof(record_header_t))

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

static const record_header_t *sector_header(const nv_region_t *r, int slot)
{
    return (const record_header_t *)(XIP_BASE + r->flash_offset + slot * FLASH_SECTOR_SIZE);
}

static bool slot_valid(const nv_region_t *r, int slot)
{
    const record_header_t *h = sector_header(r, slot);
    if (h->magic != r->magic || h->length > MAX_DATA)
        return false;
    return record_crc(h, (const uint8_t *)(h + 1)) == h->crc;
}

// Slot holding the newest valid record, or -1
static int newest_slot(const nv_region_t *r)
{
    bool v0 = slot_valid(r, 0), v1 = slot_valid(r, 1);
    if (v0 && v1)
        return (int32_t)(sector_header(r, 1)->sequence - sector_header(r, 0)->sequence) > 0;
    return v0 ? 0 : v1 ? 1 : -1;
}

bool nv_load(const nv_region_t *region, void *buf, size_t size,
             size_t *stored_len, uint16_t *version)
{
    int slot = newest_slot(region);
    if (slot < 0)
        return false;

    const record_header_t *h = sector_header(region, slot);
    size_t n = h->length < size ? h->length : size;
    memcpy(buf, h + 1, n);
    if (stored_len)
        *stored_len = h->length;
    if (version)
        *version = h->version;
    return true;
}

static uint8_t sector_buf[FLASH_SECTOR_SIZE];

typedef struct {
    uint32_t offset;
} write_ctx_t;

static void write_sector(void *param)
{
    const write_ctx_t *ctx = param;
    flash_range_erase(ctx->offset, FLASH_SECTOR_SIZE);
    flash_range_program(ctx->offset, sector_buf, FLASH_SECTOR_SIZE);
}

bool nv_save(const nv_region_t *region, const void *buf, size_t len,
             uint16_t version)
{
    if (len > MAX_DATA)
        return false;

    int newest = newest_slot(region);
    int slot = newest == 0 ? 1 : 0;
    uint32_t sequence = newest < 0 ? 1 : sector_header(region, newest)->sequence + 1;

    record_header_t h = {
        .magic = region->magic,
        .version = version,
        .length = (uint16_t)len,
        .sequence = sequence,
    };
    h.crc = record_crc(&h, buf);

    memset(sector_buf, 0xFF, sizeof(sector_buf));
    memcpy(sector_buf, &h, sizeof(h));
    memcpy(sector_buf + sizeof(h), buf, len);

    write_ctx_t ctx = { .offset = region->flash_offset + slot * FLASH_SECTOR_SIZE };
    int rc = flash_safe_execute(write_sector, &ctx, 500);
    if (rc != PICO_OK) {
        DBG("nv_save: flash_safe_execute failed (%d)\n", rc);
        return false;
    }
    return slot_valid(region, slot);
}
