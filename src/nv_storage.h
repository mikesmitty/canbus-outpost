#ifndef NV_STORAGE_H
#define NV_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Power-safe records in flash. Each region is two sectors used in turn: a
// save goes to the sector not holding the newest copy, and a CRC marks it
// complete, so losing power mid-save leaves the previous copy in place.
//
// Saving erases a sector with interrupts off for tens of milliseconds, so
// CAN frames arriving meanwhile are lost.

typedef struct {
    uint32_t flash_offset;  // offset of the first of two sectors
    uint32_t magic;
} nv_region_t;

// Regions at the top of flash
extern const nv_region_t nv_config_region;
extern const nv_region_t nv_identity_region;

// Load the newest valid record. Copies up to size bytes and reports the
// stored length and version. Returns false if neither sector is valid.
bool nv_load(const nv_region_t *region, void *buf, size_t size,
             size_t *stored_len, uint16_t *version);

bool nv_save(const nv_region_t *region, const void *buf, size_t len,
             uint16_t version);

#endif // NV_STORAGE_H
