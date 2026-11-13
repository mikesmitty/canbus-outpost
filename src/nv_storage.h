#ifndef NV_STORAGE_H
#define NV_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Power-safe records in flash. Each region is two slots used in turn: a
// save goes to the slot not holding the newest copy, and a CRC marks it
// complete, so losing power mid-save leaves the previous copy in place. A
// slot is one or more sectors, and a record fills as many as it needs.
//
// Saving erases each sector it fills with interrupts off for tens of
// milliseconds, so CAN frames arriving meanwhile are lost.
//
// The top 64 KB of flash, after partition B:
//   0x3F0000  configuration, two 16 KB slots
//   0x3F8000  spare, 16 KB
//   0x3FC000  node ID, two 4 KB slots
//   0x3FE000  configuration where earlier firmware kept it, two 4 KB slots

#define NV_SECTOR_SIZE    4096u
#define NV_HEADER_SIZE    16u
#define NV_CONFIG_SECTORS 4

// The longest record a region whose slots are this many sectors can hold
#define NV_MAX_DATA(sectors) ((sectors) * NV_SECTOR_SIZE - NV_HEADER_SIZE)

typedef struct {
    uint32_t flash_offset;  // offset of the first slot; the second follows it
    uint32_t magic;
    uint8_t slot_sectors;
} nv_region_t;

extern const nv_region_t nv_config_region;
extern const nv_region_t nv_old_config_region;
extern const nv_region_t nv_identity_region;

// Load the newest valid record. Copies up to size bytes and reports the
// stored length and version. Returns false if neither slot is valid.
bool nv_load(const nv_region_t *region, void *buf, size_t size,
             size_t *stored_len, uint16_t *version);

// Save a record, unless the newest one already holds the same data.
bool nv_save(const nv_region_t *region, const void *buf, size_t len,
             uint16_t version);

// Whether the partition table leaves the region alone. On a board whose
// table predates the top 64 KB, partition B covers the configuration's
// region. False when the table can't be read.
bool nv_region_outside_partitions(const nv_region_t *region);

#endif // NV_STORAGE_H
