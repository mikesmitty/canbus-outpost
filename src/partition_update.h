#ifndef PARTITION_UPDATE_H
#define PARTITION_UPDATE_H

#include <stdint.h>

// Brings the board's partition table up to the one this firmware was built
// with (partition_table.json), so a board gets it with a firmware upgrade
// over CAN, without USB.
//
// The boot ROM has two partition table slots, the first two 4 KB sectors of
// flash, and uses the valid table with the higher version. The new table
// goes into the slot the table in use isn't in, so the next boot uses it,
// and if the power fails while it's written, the old one stays in use. The
// table in use isn't touched, so the boot ROM's view of it holds until then.

typedef enum {
    PARTITION_UPDATE_CURRENT,  // the table in use is this firmware's
    PARTITION_UPDATE_PENDING,  // written already: the next boot uses it
    PARTITION_UPDATE_WRITTEN,  // written now: the next boot uses it
    PARTITION_UPDATE_SKIPPED,  // left alone (the log says why)
    PARTITION_UPDATE_FAILED,   // the flash write failed: tried again next boot
} partition_update_result_t;

// The partition table block made from partition_table.json when the
// firmware is built (partition_table_block.S)
extern const uint8_t partition_table_block[];
extern const uint32_t partition_table_block_size;

// Installs the partition table in block (as picotool partition create makes
// it) if the table in use differs. The workarea, word aligned and at least
// 4 KB, is for the boot ROM and the write. Interrupts are off for tens of
// milliseconds while a slot is erased and written.
partition_update_result_t partition_update(const uint8_t *block, uint32_t block_size,
                                           uint8_t *workarea, uint32_t workarea_size);

#endif // PARTITION_UPDATE_H
