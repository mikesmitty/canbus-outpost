// Runs partition_update.c on a flash array: which of the boot ROM's two
// partition table slots it writes, when it leaves the board alone, and power
// cuts while it writes. The tables are picotool's output for
// partition_table.json before (1.0) and after (1.1) the 64 KB storage area.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "partition_update.h"
#include "hardware/flash.h"
#include "pico/bootrom.h"

static int verbose;
void dbg_printf(const char *fmt, ...) { if (!verbose) return; va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }

uint8_t *mock_flash;
int mock_flash_ops;
int mock_flash_power_ops = -1;
struct mock_partition mock_partitions[PARTITION_TABLE_MAX_PARTITIONS];
int mock_npartitions;
int mock_pt_load_rc = BOOTROM_OK;

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t old_table[] = {
  0xd3, 0xde, 0xff, 0xff, 0x0a, 0x0c, 0x00, 0x02, 0x00, 0x80, 0x00, 0xfc,
  0x02, 0xc0, 0x3f, 0xfc, 0x01, 0x10, 0x02, 0xfc, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x01, 0x41, 0x00, 0x00, 0xff, 0x61, 0x7f, 0xfc,
  0x03, 0x10, 0x02, 0xfc, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x42, 0x00, 0x00, 0x48, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x47, 0x02, 0x00, 0x01, 0x11, 0x00, 0x00, 0x00, 0x4b, 0x09, 0x00, 0x00,
  0xf5, 0x17, 0x66, 0xd8, 0x0b, 0x82, 0xf2, 0xc3, 0xbb, 0x9b, 0x5f, 0x12,
  0xd1, 0xa8, 0x19, 0x7d, 0x52, 0x29, 0x3f, 0x63, 0x27, 0x98, 0xe7, 0x08,
  0x87, 0xfb, 0x7f, 0x8f, 0x4b, 0x00, 0x0c, 0xe4, 0xff, 0x19, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x79, 0x35, 0x12, 0xab
};

static const uint8_t new_table[] = {
  0xd3, 0xde, 0xff, 0xff, 0x0a, 0x0c, 0x00, 0x02, 0x00, 0x80, 0x00, 0xfc,
  0x02, 0xc0, 0x3f, 0xfc, 0x01, 0x10, 0x02, 0xfc, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x01, 0x41, 0x00, 0x00, 0xff, 0xe1, 0x7d, 0xfc,
  0x03, 0x10, 0x02, 0xfc, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x42, 0x00, 0x00, 0x48, 0x02, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
  0x47, 0x02, 0x00, 0x01, 0x11, 0x00, 0x00, 0x00, 0x4b, 0x09, 0x00, 0x00,
  0x67, 0xea, 0x94, 0xec, 0xc2, 0xf6, 0xe1, 0xe0, 0xb2, 0x70, 0xb2, 0x85,
  0xb9, 0x5c, 0x31, 0x76, 0x63, 0x43, 0x5b, 0xda, 0x3b, 0x8f, 0x09, 0x70,
  0x7d, 0x9a, 0x85, 0x61, 0xa4, 0x9c, 0x5c, 0x57, 0xff, 0x19, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x79, 0x35, 0x12, 0xab
};

#define TABLE_SIZE  sizeof(new_table)
#define SLOT(s)     ((s) * FLASH_SECTOR_SIZE)
#define VERSION_AT  56  // the VERSION item's major.minor word
#define LOCATION(p) (12 + 20 * (p))  // partition p's location word, then flags

static uint8_t workarea[4096];
static uint8_t junk[FLASH_SECTOR_SIZE];  // say, firmware from before the partition table

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void blank_flash(void)
{
    memset(mock_flash, 0xFF, MOCK_FLASH_SIZE);
    mock_flash_ops = 0;
    mock_flash_power_ops = -1;
    mock_pt_load_rc = BOOTROM_OK;
}

static void put_slot(int slot, const uint8_t *data, size_t len)
{
    memset(mock_flash + SLOT(slot), 0xFF, FLASH_SECTOR_SIZE);
    memcpy(mock_flash + SLOT(slot), data, len);
}

// The boot ROM loaded this table: its two partitions, as picotool lays
// them out (each with an ID and a one-letter name)
static void rom_loaded(const uint8_t *table)
{
    for (int p = 0; p < 2; p++) {
        uint32_t loc = rd32(table + LOCATION(p));
        mock_partitions[p] = (struct mock_partition){
            .first_sector = loc & 0x1fff,
            .last_sector = (loc >> 13) & 0x1fff,
            .permissions = loc & 0xfc000000u,
            .flags = rd32(table + LOCATION(p) + 4),
        };
    }
    mock_npartitions = 2;
}

static partition_update_result_t update(void)
{
    return partition_update(new_table, TABLE_SIZE, workarea, sizeof(workarea));
}

// The slot holds the table, then erased flash to the end of its sector
static bool slot_holds(int slot, const uint8_t *table)
{
    if (memcmp(mock_flash + SLOT(slot), table, TABLE_SIZE) != 0)
        return false;
    for (uint32_t i = TABLE_SIZE; i < FLASH_SECTOR_SIZE; i++)
        if (mock_flash[SLOT(slot) + i] != 0xFF)
            return false;
    return true;
}

static void test_fixtures(void)
{
    // A from 8K, B after it to the end of the old or new table's space
    rom_loaded(old_table);
    CHECK(mock_partitions[0].first_sector == 2 && mock_partitions[0].last_sector == 510, "old A");
    CHECK(mock_partitions[1].first_sector == 511 && mock_partitions[1].last_sector == 1019, "old B");
    rom_loaded(new_table);
    CHECK(mock_partitions[1].first_sector == 511 && mock_partitions[1].last_sector == 1007, "new B");
    CHECK(rd32(old_table + VERSION_AT) == 0x00010000 && rd32(new_table + VERSION_AT) == 0x00010001,
          "versions 1.0 and 1.1");
}

static void test_current(void)
{
    // Set up over USB with the new table
    blank_flash();
    put_slot(0, new_table, TABLE_SIZE);
    rom_loaded(new_table);
    CHECK(update() == PARTITION_UPDATE_CURRENT, "new table in use");
    CHECK(mock_flash_ops == 0, "flash written");

    // The same partitions with other flags (B taking another family) differ
    uint8_t t[TABLE_SIZE];
    memcpy(t, new_table, TABLE_SIZE);
    wr32(t + VERSION_AT, 0x00010000);
    wr32(t + LOCATION(1) + 4, rd32(t + LOCATION(1) + 4) ^ 0x00040000);
    put_slot(0, t, TABLE_SIZE);
    rom_loaded(t);
    CHECK(update() == PARTITION_UPDATE_WRITTEN, "other flags not written");
    CHECK(slot_holds(1, new_table), "slot 1 doesn't hold the new table");
}

static void test_upgrade(void)
{
    // A board set up with the old table: slot 1 erased
    blank_flash();
    put_slot(0, old_table, TABLE_SIZE);
    rom_loaded(old_table);
    CHECK(update() == PARTITION_UPDATE_WRITTEN, "not written");
    CHECK(slot_holds(1, new_table), "slot 1 doesn't hold the new table");
    CHECK(slot_holds(0, old_table), "slot 0 changed");
    CHECK(mock_flash_ops == 2, "%d flash operations, expected an erase and a program", mock_flash_ops);

    // Again before the reboot: nothing more to do
    CHECK(update() == PARTITION_UPDATE_PENDING, "second call");
    CHECK(mock_flash_ops == 2, "written twice");

    // After it the boot ROM uses slot 1's newer table
    rom_loaded(new_table);
    CHECK(update() == PARTITION_UPDATE_CURRENT, "after the reboot");
    CHECK(mock_flash_ops == 2, "written after the reboot");
}

static void test_other_slot(void)
{
    // The table in use in slot 1: slot 0 is written, whatever it held
    blank_flash();
    put_slot(0, junk, sizeof(junk));
    put_slot(1, old_table, TABLE_SIZE);
    rom_loaded(old_table);
    CHECK(update() == PARTITION_UPDATE_WRITTEN, "not written");
    CHECK(slot_holds(0, new_table), "slot 0 doesn't hold the new table");
    CHECK(slot_holds(1, old_table), "slot 1 changed");

    // The table in use in slot 0 and leftovers in slot 1
    blank_flash();
    put_slot(0, old_table, TABLE_SIZE);
    put_slot(1, junk, sizeof(junk));
    rom_loaded(old_table);
    CHECK(update() == PARTITION_UPDATE_WRITTEN, "not written over leftovers");
    CHECK(slot_holds(1, new_table), "slot 1 doesn't hold the new table");
    CHECK(slot_holds(0, old_table), "slot 0 changed");
}

static void test_power_cuts(void)
{
    // Power off before the erase, between it and the program, or in the
    // middle of the program (part of the block written): slot 0 keeps the
    // table in use, and the next boot writes slot 1 again
    for (int cut = 0; cut < 3; cut++) {
        blank_flash();
        put_slot(0, old_table, TABLE_SIZE);
        put_slot(1, junk, sizeof(junk));
        rom_loaded(old_table);
        if (cut < 2) {
            mock_flash_power_ops = cut;
            CHECK(update() == PARTITION_UPDATE_FAILED, "cut %d: not reported", cut);
        } else {
            put_slot(1, new_table, TABLE_SIZE / 2);
        }
        CHECK(slot_holds(0, old_table), "cut %d: slot 0 changed", cut);
        CHECK(!slot_holds(1, new_table), "cut %d: slot 1 complete", cut);

        mock_flash_power_ops = -1;
        CHECK(update() == PARTITION_UPDATE_WRITTEN, "cut %d: not written after it", cut);
        CHECK(slot_holds(1, new_table), "cut %d: slot 1 doesn't hold the new table", cut);
        CHECK(slot_holds(0, old_table), "cut %d: slot 0 changed after it", cut);
    }
}

// Each case leaves the flash as it was
static void check_skipped(const char *what)
{
    uint8_t before[2 * FLASH_SECTOR_SIZE];
    memcpy(before, mock_flash, sizeof(before));
    CHECK(update() == PARTITION_UPDATE_SKIPPED, "%s: not skipped", what);
    CHECK(mock_flash_ops == 0 && memcmp(before, mock_flash, sizeof(before)) == 0,
          "%s: flash written", what);
}

static void test_left_alone(void)
{
    uint8_t t[TABLE_SIZE];

    // A singleton table: the boot ROM wouldn't read the other slot
    blank_flash();
    memcpy(t, old_table, TABLE_SIZE);
    t[7] |= 0x80;
    put_slot(0, t, TABLE_SIZE);
    rom_loaded(t);
    check_skipped("singleton");

    // A table in use as new as this one, or newer (firmware downgraded):
    // the boot ROM would keep it
    for (int v = 0; v < 2; v++) {
        blank_flash();
        memcpy(t, old_table, TABLE_SIZE);
        wr32(t + VERSION_AT, v ? 0x00020000 : 0x00010001);
        put_slot(0, t, TABLE_SIZE);
        rom_loaded(t);
        check_skipped(v ? "newer table in use" : "same version in use");
    }

    // No partition table, or none loaded
    blank_flash();
    put_slot(0, junk, sizeof(junk));
    mock_npartitions = 0;
    check_skipped("no table");
    blank_flash();
    put_slot(0, old_table, TABLE_SIZE);
    rom_loaded(old_table);
    mock_pt_load_rc = BOOTROM_ERROR_NOT_FOUND;
    check_skipped("not loaded");

    // Neither slot holds the table in use
    blank_flash();
    put_slot(1, junk, sizeof(junk));
    rom_loaded(old_table);
    check_skipped("table in use not in a slot");

    // Both do
    blank_flash();
    put_slot(0, old_table, TABLE_SIZE);
    put_slot(1, old_table, TABLE_SIZE);
    rom_loaded(old_table);
    check_skipped("both slots");

    // A partition starting in slot 1
    blank_flash();
    memcpy(t, old_table, TABLE_SIZE);
    wr32(t + LOCATION(0), (rd32(t + LOCATION(0)) & ~0x1fffu) | 1);
    put_slot(0, t, TABLE_SIZE);
    rom_loaded(t);
    check_skipped("partition in the slots");
}

static void test_damaged_built_in(void)
{
    uint8_t t[TABLE_SIZE + 4];
    blank_flash();
    put_slot(0, old_table, TABLE_SIZE);
    rom_loaded(old_table);

    struct { const char *what; size_t len; int at; uint8_t value; } cases[] = {
        { "truncated", TABLE_SIZE - 4, -1, 0 },
        { "end marker", TABLE_SIZE, TABLE_SIZE - 1, 0x00 },
        { "start marker", TABLE_SIZE, 0, 0x00 },
        { "not a partition table", TABLE_SIZE, 4, 0x0b },
        { "last item's size", TABLE_SIZE, TABLE_SIZE - 11, 0x18 },
        { "partition count", TABLE_SIZE, 7, 0x10 },
        { "trailing bytes", TABLE_SIZE + 4, -1, 0 },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        memset(t, 0, sizeof(t));
        memcpy(t, new_table, TABLE_SIZE);
        if (cases[c].at >= 0)
            t[cases[c].at] = cases[c].value;
        uint8_t before[2 * FLASH_SECTOR_SIZE];
        memcpy(before, mock_flash, sizeof(before));
        CHECK(partition_update(t, cases[c].len, workarea, sizeof(workarea)) ==
                  PARTITION_UPDATE_SKIPPED, "%s: not skipped", cases[c].what);
        CHECK(memcmp(before, mock_flash, sizeof(before)) == 0, "%s: flash written", cases[c].what);
    }

    // Workarea too small for the write
    CHECK(partition_update(new_table, TABLE_SIZE, workarea, 128) == PARTITION_UPDATE_SKIPPED,
          "small workarea");
    CHECK(mock_flash_ops == 0, "written with a small workarea");
}

// A table in use whose partition has extra families and a longer name: the
// walk over its entries finds it in slot 0
static void test_entry_walk(void)
{
    enum { HAS_ID = 1, TWO_FAMILIES = 2 << 7, HAS_NAME = 0x1000 };
    uint32_t loc = 0xfc000000u | 2 | 100u << 13;
    uint32_t flags = 0xfc000000u | HAS_ID | TWO_FAMILIES | HAS_NAME;
    uint32_t words[] = {
        0xffffded3u,
        0x0a | 11 << 8 | 1u << 24,  // a partition table item of 11 words, one partition
        0xfc000000u,                // unpartitioned space
        loc, flags,
        0x12345678u, 0x9abcdef0u,   // ID
        0xe48bff56u, 0xe48bff57u,   // extra families
        0x72694608u, 0x7261776du, 0x00000065u,  // name: 8, "Firmware"
        0x48 | 2 << 8, 0x00010000u, // version 1.0
        0xff | 13 << 8,             // last
        0,
        0xab123579u,
    };
    uint8_t t[sizeof(words)];
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        wr32(t + 4 * i, words[i]);

    blank_flash();
    put_slot(0, t, sizeof(t));
    mock_partitions[0] = (struct mock_partition){ 2, 100, 0xfc000000u, flags };
    mock_npartitions = 1;
    CHECK(update() == PARTITION_UPDATE_WRITTEN, "not written");
    CHECK(slot_holds(1, new_table), "slot 1 doesn't hold the new table");

    // A 12-byte name takes another word, which isn't there
    blank_flash();
    wr32(t + 4 * 9, 0x7269460cu);
    put_slot(0, t, sizeof(t));
    check_skipped("name past the item");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    mock_flash = malloc(MOCK_FLASH_SIZE);
    for (size_t i = 0; i < sizeof(junk); i++)
        junk[i] = (uint8_t)(i * 37 + (i >> 4));

    test_fixtures();
    test_current();
    test_upgrade();
    test_other_slot();
    test_power_cuts();
    test_left_alone();
    test_damaged_built_in();
    test_entry_walk();

    free(mock_flash);
    printf("partition update: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
