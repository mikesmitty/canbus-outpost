#include "nv_storage.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include <string.h>
#include <stdio.h>

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

#define FLASH_CONFIG_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define FLASH_TARGET_CONTENTS ((const uint8_t *)(XIP_BASE + FLASH_CONFIG_OFFSET))

bool nv_storage_init(void *buffer, size_t size) {
    if (size > FLASH_SECTOR_SIZE) return false;

    // Check if flash is erased (all 0xFF)
    bool erased = true;
    for (size_t i = 0; i < size; i++) {
        if (FLASH_TARGET_CONTENTS[i] != 0xFF) {
            erased = false;
            break;
        }
    }

    if (!erased) {
        memcpy(buffer, FLASH_TARGET_CONTENTS, size);
        return true;
    }

    return false;
}

static uint8_t page_buffer[FLASH_SECTOR_SIZE];

typedef struct {
    const uint8_t *data;
    uint32_t prog_size;
} flash_write_ctx_t;

static void flash_write_callback(void *param) {
    flash_write_ctx_t *ctx = (flash_write_ctx_t *)param;
    flash_range_erase(FLASH_CONFIG_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_CONFIG_OFFSET, ctx->data, ctx->prog_size);
}

bool nv_storage_write(const void *buffer, size_t size) {
    if (size > FLASH_SECTOR_SIZE) return false;

    uint32_t prog_size = (size + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    memset(page_buffer, 0xFF, FLASH_SECTOR_SIZE);
    memcpy(page_buffer, buffer, size);

    flash_write_ctx_t ctx = {
        .data = page_buffer,
        .prog_size = FLASH_SECTOR_SIZE, // Always program a full sector for simplicity
    };

    // Use flash_safe_execute to handle XIP and interrupts
    int rc = flash_safe_execute(flash_write_callback, &ctx, 500);
    
    return rc == PICO_OK;
}
