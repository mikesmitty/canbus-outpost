#include "partition_update.h"
#include <stdbool.h>
#include <string.h>
#include "hardware/flash.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "boot/picobin.h"
#include "util/dbg.h"

#define SLOTS      2
#define SLOT_SIZE  FLASH_SECTOR_SIZE

#define PT_PRESENT 0x100  // in the partition count word of PT_INFO

typedef struct {
    bool singleton;     // the boot ROM ignores the other slot
    uint32_t version;   // major << 16 | minor
    uint32_t size;      // bytes, through the end marker
    int count;
    uint32_t location[PARTITION_TABLE_MAX_PARTITIONS];
    uint32_t flags[PARTITION_TABLE_MAX_PARTITIONS];
} table_t;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t item_size(uint32_t header)
{
    return (header & 0x80) ? (header >> 8) & 0xffff : (header >> 8) & 0xff;
}

// A block whose first item is a partition table (as the boot ROM requires),
// at the start of p. Hashes aren't checked: the boot ROM does that.
static bool parse_table(const uint8_t *p, uint32_t len, table_t *t)
{
#define W(i) rd32(p + 4 * (i))
    uint32_t words = len / 4;
    memset(t, 0, sizeof(*t));
    if (words < 2 || W(0) != PICOBIN_BLOCK_MARKER_START)
        return false;

    uint32_t header = W(1), size = item_size(header);
    t->singleton = (header >> 31) != 0;
    t->count = (header >> 24) & 0x7f;
    if ((header & 0x7f) != PICOBIN_BLOCK_ITEM_PARTITION_TABLE || size < 2 || size >= words ||
        t->count > PARTITION_TABLE_MAX_PARTITIONS)
        return false;

    // After the item header and the unpartitioned space's permissions, each
    // partition: location and flags, then an ID, extra families and a name
    // as the flags say
    uint32_t i = 2;
    for (int n = 0; n < t->count; n++) {
        if (i + 2 > size)
            return false;
        uint32_t flags = W(1 + i + 1);
        t->location[n] = W(1 + i);
        t->flags[n] = flags;
        i += 2;
        if (flags & PICOBIN_PARTITION_FLAGS_HAS_ID_BITS)
            i += 2;
        i += (flags & PICOBIN_PARTITION_FLAGS_ACCEPTS_NUM_EXTRA_FAMILIES_BITS) >>
             PICOBIN_PARTITION_FLAGS_ACCEPTS_NUM_EXTRA_FAMILIES_LSB;
        if (flags & PICOBIN_PARTITION_FLAGS_HAS_NAME_BITS) {
            if (i >= size)
                return false;
            i += ((W(1 + i) & 0x7f) + 4) / 4;  // a length byte, then the name
        }
    }
    if (i != size)
        return false;

    // The other items, the version among them, then the last item (its size
    // the total of the items before it), a link and the end marker
    for (uint32_t w = 1 + size;;) {
        if (w + 3 > words)
            return false;
        uint32_t h = W(w), s = item_size(h);
        if ((h & 0x7f) == (PICOBIN_BLOCK_ITEM_2BS_LAST & 0x7f)) {
            if (s != w - 1 || W(w + 2) != PICOBIN_BLOCK_MARKER_END)
                return false;
            t->size = (w + 3) * 4;
            return true;
        }
        if (s == 0)
            return false;
        if ((h & 0x7f) == PICOBIN_BLOCK_ITEM_1BS_VERSION && s >= 2)
            t->version = W(w + 1);
        w += s;
    }
#undef W
}

// A partition table in a slot, its block the first in the slot
static bool slot_table(int slot, table_t *t)
{
    const uint8_t *p = (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + slot * SLOT_SIZE);
    for (uint32_t at = 0; at + 4 <= SLOT_SIZE; at += 4)
        if (rd32(p + at) == PICOBIN_BLOCK_MARKER_START)
            return parse_table(p + at, SLOT_SIZE - at, t);
    return false;
}

// The partitions of the table the boot ROM loaded
static bool table_in_use(uint8_t *workarea, uint32_t workarea_size, table_t *t)
{
    memset(t, 0, sizeof(*t));
    int rc = rom_load_partition_table(workarea, workarea_size, false);
    if (rc != BOOTROM_OK)
        return false;

    // The flags supported, three words about the table, then two per partition
    static uint32_t info[1 + 3 + 2 * PARTITION_TABLE_MAX_PARTITIONS];
    const uint32_t flags = PT_INFO_PT_INFO | PT_INFO_PARTITION_LOCATION_AND_FLAGS;
    int words = rom_get_partition_table_info(info, sizeof(info) / sizeof(info[0]), flags);
    if (words < 4 || info[0] != flags || !(info[1] & PT_PRESENT))
        return false;
    t->count = info[1] & 0xff;
    if (t->count > PARTITION_TABLE_MAX_PARTITIONS || words < 4 + 2 * t->count)
        return false;
    for (int n = 0; n < t->count; n++) {
        t->location[n] = info[4 + 2 * n];
        t->flags[n] = info[5 + 2 * n];
    }
    return true;
}

static bool same_partitions(const table_t *a, const table_t *b)
{
    if (a->count != b->count)
        return false;
    for (int n = 0; n < a->count; n++)
        if (a->location[n] != b->location[n] || a->flags[n] != b->flags[n])
            return false;
    return true;
}

// No partition reaches into the partition table slots
static bool clear_of_slots(const table_t *t)
{
    for (int n = 0; n < t->count; n++) {
        uint32_t first = (t->location[n] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                         PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
        if (first < SLOTS)
            return false;
    }
    return true;
}

typedef struct {
    uint32_t offset;
    const uint8_t *data;  // in RAM: flash can't be read while it's written
    uint32_t len;
} slot_write_t;

static void write_slot(void *param)
{
    const slot_write_t *w = param;
    flash_range_erase(w->offset, SLOT_SIZE);
    flash_range_program(w->offset, w->data, w->len);
}

#define MAJOR(v) ((unsigned)((v) >> 16))
#define MINOR(v) ((unsigned)((v) & 0xffff))

partition_update_result_t partition_update(const uint8_t *block, uint32_t block_size,
                                           uint8_t *workarea, uint32_t workarea_size)
{
    table_t want, in_use, slot[SLOTS];
    uint32_t len = (block_size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
    if (block_size > SLOT_SIZE || len > workarea_size || !parse_table(block, block_size, &want) ||
        want.size != block_size) {
        DBG("partition table: the one built in is damaged\n");
        return PARTITION_UPDATE_SKIPPED;
    }
    if (!table_in_use(workarea, workarea_size, &in_use)) {
        DBG("partition table: none in use\n");
        return PARTITION_UPDATE_SKIPPED;
    }
    if (same_partitions(&in_use, &want)) {
        DBG("partition table: version %u.%u in use\n", MAJOR(want.version), MINOR(want.version));
        return PARTITION_UPDATE_CURRENT;
    }

    // The slot holding the table in use, which mustn't be touched
    int current = -1, matches = 0;
    for (int s = 0; s < SLOTS; s++) {
        const uint8_t *p = (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + s * SLOT_SIZE);
        if (memcmp(p, block, block_size) == 0) {
            DBG("partition table: version %u.%u written to slot %d, the next boot uses it\n",
                MAJOR(want.version), MINOR(want.version), s);
            return PARTITION_UPDATE_PENDING;
        }
        if (slot_table(s, &slot[s]) && same_partitions(&slot[s], &in_use)) {
            current = s;
            matches++;
        }
    }
    if (matches != 1) {
        DBG("partition table: can't tell which slot holds the one in use\n");
        return PARTITION_UPDATE_SKIPPED;
    }
    if (slot[current].singleton) {
        DBG("partition table: the one in use is a singleton, so the boot ROM won't read "
            "the other slot (install the new one with make partition-table)\n");
        return PARTITION_UPDATE_SKIPPED;
    }
    if (want.version <= slot[current].version) {
        DBG("partition table: the one in use, version %u.%u, is as new as %u.%u\n",
            MAJOR(slot[current].version), MINOR(slot[current].version),
            MAJOR(want.version), MINOR(want.version));
        return PARTITION_UPDATE_SKIPPED;
    }
    if (!clear_of_slots(&in_use) || !clear_of_slots(&want)) {
        DBG("partition table: a partition overlaps the partition table slots\n");
        return PARTITION_UPDATE_SKIPPED;
    }

    int target = SLOTS - 1 - current;
    memset(workarea, 0xFF, len);
    memcpy(workarea, block, block_size);
    slot_write_t w = { .offset = target * SLOT_SIZE, .data = workarea, .len = len };
    int rc = flash_safe_execute(write_slot, &w, 500);
    const uint8_t *written = (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + w.offset);
    if (rc != PICO_OK || memcmp(written, workarea, len) != 0) {
        DBG("partition table: writing slot %d failed (%d)\n", target, rc);
        return PARTITION_UPDATE_FAILED;
    }
    DBG("partition table: wrote version %u.%u to slot %d, the next boot uses it\n",
        MAJOR(want.version), MINOR(want.version), target);
    return PARTITION_UPDATE_WRITTEN;
}
