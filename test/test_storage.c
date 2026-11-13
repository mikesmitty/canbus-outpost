// Runs nv_storage.c and config.c on a flash array: records spanning several
// sectors, power cuts while saving, and the move from the old storage area
// once the partition table leaves the new one free.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/config.h"
#include "nv_storage.h"
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

#define KB 1024u
#define SECTOR(kb) ((kb) * KB / FLASH_SECTOR_SIZE)

// A and B as partition_table.json has them, B ending at the given KB
static void partitions(unsigned b_end_kb)
{
    mock_partitions[0] = (struct mock_partition){ .first_sector = SECTOR(8),
                                                 .last_sector = SECTOR(2044) - 1 };
    mock_partitions[1] = (struct mock_partition){ .first_sector = SECTOR(2044),
                                                 .last_sector = SECTOR(b_end_kb) - 1 };
    mock_npartitions = 2;
    mock_pt_load_rc = BOOTROM_OK;
}
#define OLD_TABLE 4080  // before the 64 KB storage area
#define NEW_TABLE 4032

static void blank_flash(void)
{
    memset(mock_flash, 0xFF, MOCK_FLASH_SIZE);
    mock_flash_ops = 0;
    mock_flash_power_ops = -1;
}

static void fill(uint8_t *buf, size_t len, unsigned seed)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(i * 31 + seed * 7 + (i >> 8));
}

static bool flash_untouched(uint32_t offset, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (mock_flash[offset + i] != 0xFF)
            return false;
    return true;
}

static void test_region_choice(void)
{
    partitions(NEW_TABLE);
    CHECK(nv_region_outside_partitions(&nv_config_region), "new table: config region overlaps");
    CHECK(nv_region_outside_partitions(&nv_old_config_region), "new table: old region overlaps");
    CHECK(nv_region_outside_partitions(&nv_identity_region), "new table: identity overlaps");

    partitions(OLD_TABLE);
    CHECK(!nv_region_outside_partitions(&nv_config_region), "old table: B covers the config region");
    CHECK(nv_region_outside_partitions(&nv_old_config_region), "old table: old region overlaps");
    CHECK(nv_region_outside_partitions(&nv_identity_region), "old table: identity overlaps");

    // One sector into the region is still an overlap
    partitions(NEW_TABLE + 4);
    CHECK(!nv_region_outside_partitions(&nv_config_region), "B one sector in");

    // A partition on the region's last sector (its second slot's), or just
    // past it
    partitions(NEW_TABLE);
    mock_partitions[2] = (struct mock_partition){ .first_sector = SECTOR(4064) - 1,
                                                 .last_sector = SECTOR(4064) - 1 };
    mock_npartitions = 3;
    CHECK(!nv_region_outside_partitions(&nv_config_region), "partition on the last sector");
    mock_partitions[2] = (struct mock_partition){ .first_sector = SECTOR(4064),
                                                 .last_sector = SECTOR(4064) };
    CHECK(nv_region_outside_partitions(&nv_config_region), "partition past the region");

    mock_npartitions = 0;
    CHECK(nv_region_outside_partitions(&nv_config_region), "no partitions");

    partitions(NEW_TABLE);
    mock_pt_load_rc = BOOTROM_ERROR_NOT_FOUND;
    CHECK(!nv_region_outside_partitions(&nv_config_region), "table unreadable but region used");
}

static void test_layout(void)
{
    // Inside the top 64 KB, clear of each other
    CHECK(nv_config_region.flash_offset == 0x3F0000, "config at 0x%x", (unsigned)nv_config_region.flash_offset);
    CHECK(nv_config_region.flash_offset + 2 * NV_CONFIG_SECTORS * FLASH_SECTOR_SIZE <= 0x3F8000,
          "config region runs into the spare 16 KB");
    CHECK(nv_identity_region.flash_offset == 0x3FC000, "the node ID moved");
    CHECK(nv_old_config_region.flash_offset == 0x3FE000, "the old config region moved");
    CHECK(sizeof(config_layout_t) <= NV_MAX_DATA(NV_CONFIG_SECTORS), "config doesn't fit");
}

static void test_multi_sector_records(void)
{
    static uint8_t a[NV_MAX_DATA(NV_CONFIG_SECTORS)], b[sizeof(a)], out[sizeof(a)];
    const nv_region_t *r = &nv_config_region;
    size_t len;
    uint16_t version;

    blank_flash();
    CHECK(!nv_load(r, out, sizeof(out), &len, &version), "blank flash has a record");

    fill(a, 10000, 1);
    CHECK(nv_save(r, a, 10000, 3), "save 10000 bytes");
    CHECK(mock_flash_ops == 2 * 3, "%d flash ops for three sectors", mock_flash_ops);
    memset(out, 0, sizeof(out));
    CHECK(nv_load(r, out, sizeof(out), &len, &version) && len == 10000 && version == 3 &&
          memcmp(out, a, 10000) == 0, "10000 bytes didn't come back");

    // The next save goes in the other slot; the newest wins
    fill(b, sizeof(b), 2);
    CHECK(nv_save(r, b, sizeof(b), 3), "save the largest record");
    CHECK(nv_load(r, out, sizeof(out), &len, NULL) && len == sizeof(b) && memcmp(out, b, sizeof(b)) == 0,
          "largest record didn't come back");

    // A shorter record over a slot that held a longer one
    fill(a, 100, 3);
    CHECK(nv_save(r, a, 100, 3), "save 100 bytes");
    CHECK(nv_load(r, out, sizeof(out), &len, NULL) && len == 100 && memcmp(out, a, 100) == 0,
          "100 bytes after a long record");

    // Unchanged: no erase
    int ops = mock_flash_ops;
    CHECK(nv_save(r, a, 100, 3), "save unchanged");
    CHECK(mock_flash_ops == ops, "an unchanged save wrote flash");

    // Too long for the region
    CHECK(!nv_save(r, a, sizeof(a) + 1, 3), "saved more than a slot holds");
    CHECK(!nv_save(&nv_old_config_region, a, NV_MAX_DATA(1) + 1, 3), "saved more than an old slot holds");

    // Neighbors untouched
    CHECK(flash_untouched(0x3F8000, 0x8000), "wrote outside the config region");
}

// Cut the power after every number of flash operations a save takes: the
// record is the old one or the new one, never anything else, and the next
// save works
static void test_power_cuts(void)
{
    static uint8_t a[9000], b[9000], c[9000], out[9000];
    const nv_region_t *r = &nv_config_region;
    fill(a, sizeof(a), 4);
    fill(b, sizeof(b), 5);
    fill(c, sizeof(c), 6);

    // 9000 bytes and the header fill three sectors: an erase and a program each
    for (int cut = 0; cut <= 6; cut++) {
        blank_flash();
        CHECK(nv_save(r, a, sizeof(a), 1) && nv_save(r, b, sizeof(b), 1) && nv_save(r, a, sizeof(a), 1),
              "first saves");

        // c over the slot holding b
        mock_flash_ops = 0;
        mock_flash_power_ops = cut;
        nv_save(r, c, sizeof(c), 1);
        mock_flash_power_ops = -1;

        size_t len;
        memset(out, 0, sizeof(out));
        bool ok = nv_load(r, out, sizeof(out), &len, NULL);
        bool old = ok && memcmp(out, a, sizeof(a)) == 0;
        bool new = ok && memcmp(out, c, sizeof(c)) == 0;
        CHECK(ok && len == sizeof(a) && (old || new), "cut after %d ops: no good record", cut);
        CHECK(cut == 6 ? new : old, "cut after %d ops: %s record", cut, new ? "new" : "old");

        CHECK(nv_save(r, b, sizeof(b), 1) && nv_load(r, out, sizeof(out), &len, NULL) &&
              memcmp(out, b, sizeof(b)) == 0, "cut after %d ops: next save failed", cut);
    }
}

static void test_config_old_table(void)
{
    blank_flash();
    partitions(OLD_TABLE);
    CHECK(config_load() == 0, "blank flash loaded a config");
    fill((uint8_t *)&config, sizeof(config), 6);
    CHECK(config_save(), "config save on the old table");
    CHECK(flash_untouched(nv_config_region.flash_offset, 2 * NV_CONFIG_SECTORS * FLASH_SECTOR_SIZE),
          "old table: wrote inside partition B");

    static config_layout_t saved;
    saved = config;
    memset(&config, 0, sizeof(config));
    CHECK(config_load() == sizeof(config) && memcmp(&config, &saved, sizeof(config)) == 0,
          "old table: config didn't come back");
}

static void test_config_moves_across(void)
{
    // Earlier firmware saved a shorter configuration in the old region
    static uint8_t old[3192];
    fill(old, sizeof(old), 7);
    blank_flash();
    CHECK(nv_save(&nv_old_config_region, old, sizeof(old), CONFIG_VERSION), "old save");

    partitions(NEW_TABLE);
    CHECK(config_load() == sizeof(old), "didn't load the old region's record");
    CHECK(memcmp(&config, old, sizeof(old)) == 0, "old record's contents");

    // The next save lands in the new region, and the old copy stays
    fill((uint8_t *)&config + sizeof(old), sizeof(config) - sizeof(old), 8);
    CHECK(config_save(), "save after moving");
    static config_layout_t saved;
    saved = config;
    size_t len;
    static uint8_t buf[sizeof(config_layout_t)];
    CHECK(nv_load(&nv_config_region, buf, sizeof(buf), &len, NULL) && len == sizeof(config) &&
          memcmp(buf, &saved, sizeof(config)) == 0, "the new region doesn't hold it");
    CHECK(nv_load(&nv_old_config_region, buf, sizeof(buf), &len, NULL) && len == sizeof(old) &&
          memcmp(buf, old, sizeof(old)) == 0, "the old copy changed");

    // From now on the new region is the one read
    memset(&config, 0, sizeof(config));
    CHECK(config_load() == sizeof(config) && memcmp(&config, &saved, sizeof(config)) == 0,
          "after moving, config didn't come back");
}

static void test_config_version(void)
{
    blank_flash();
    partitions(NEW_TABLE);
    fill((uint8_t *)&config, sizeof(config), 9);
    CHECK(nv_save(&nv_config_region, &config, sizeof(config), CONFIG_VERSION + 1), "save");
    CHECK(config_load() == 0, "loaded another version's config");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    mock_flash = malloc(MOCK_FLASH_SIZE);
    test_region_choice();
    test_layout();
    test_multi_sector_records();
    test_power_cuts();
    test_config_old_table();
    test_config_moves_across();
    test_config_version();
    printf("storage: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
