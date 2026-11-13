// Stand-in for the SDK's boot ROM header: the partition table in use is
// whatever the test puts in mock_partitions.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "boot/picobin.h"

#define BOOTROM_OK                      0
#define BOOTROM_ERROR_NOT_FOUND         (-17)
#define PT_INFO_PT_INFO                 0x0001
#define PT_INFO_PARTITION_LOCATION_AND_FLAGS 0x0010
#define PARTITION_TABLE_MAX_PARTITIONS  16

// Permission bits as in the location word (its top six bits)
struct mock_partition { uint32_t first_sector, last_sector, permissions, flags; };
extern struct mock_partition mock_partitions[PARTITION_TABLE_MAX_PARTITIONS];
extern int mock_npartitions;
extern int mock_pt_load_rc;  // what loading the table returns

static inline int rom_load_partition_table(uint8_t *work, uint32_t size, bool force)
{
    (void)work; (void)size; (void)force;
    return mock_pt_load_rc;
}

static inline int rom_get_partition_table_info(uint32_t *out, uint32_t words, uint32_t flags)
{
    uint32_t n = 0;
    if (words < 4 + 2 * (uint32_t)mock_npartitions)
        return -13;
    out[n++] = flags & (PT_INFO_PT_INFO | PT_INFO_PARTITION_LOCATION_AND_FLAGS);
    if (flags & PT_INFO_PT_INFO) {
        out[n++] = (uint32_t)mock_npartitions | (mock_npartitions ? 0x100 : 0);  // present
        out[n++] = 0;
        out[n++] = 0;
    }
    if (flags & PT_INFO_PARTITION_LOCATION_AND_FLAGS)
        for (int p = 0; p < mock_npartitions; p++) {
            out[n++] = mock_partitions[p].permissions |
                       mock_partitions[p].first_sector << PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB |
                       mock_partitions[p].last_sector << PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
            out[n++] = mock_partitions[p].flags;
        }
    return (int)n;
}
