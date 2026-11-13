// Stand-in for the SDK's flash header: flash is an array the test owns,
// read through the XIP address, and a power cut can be set to stop all
// erases and programs after a number of them.
#pragma once
#include <stdint.h>
#include <string.h>

#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE   256u
#define MOCK_FLASH_SIZE   (4 * 1024 * 1024)

extern uint8_t *mock_flash;  // MOCK_FLASH_SIZE bytes
extern int mock_flash_ops;        // erases and programs done
extern int mock_flash_power_ops;  // the power fails after this many; -1 = never

#define XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE ((uintptr_t)mock_flash)

static inline int mock_flash_powered(void)
{
    if (mock_flash_power_ops >= 0 && mock_flash_ops >= mock_flash_power_ops)
        return 0;
    mock_flash_ops++;
    return 1;
}

static inline void flash_range_erase(uint32_t offset, size_t count)
{
    if (mock_flash_powered())
        memset(mock_flash + offset, 0xFF, count);
}

// NOR flash: programming only clears bits
static inline void flash_range_program(uint32_t offset, const uint8_t *data, size_t count)
{
    if (mock_flash_powered())
        for (size_t i = 0; i < count; i++)
            mock_flash[offset + i] &= data[i];
}
