#include "fw_image.h"
#include <stddef.h>

// From pico-sdk boot/picobin.h (not included so this builds on the host)
#define BLOCK_MARKER_START   0xffffded3u
#define BLOCK_MARKER_END     0xab123579u
#define ITEM_IMAGE_TYPE      0x42
#define ITEM_HASH_VALUE      0x4b
#define ITEM_LAST            0xff
#define ITEM_2BS             0x80   // size is two bytes, not one

#define IMAGE_TYPE_MASK      0x000f
#define IMAGE_TYPE_EXE       0x0001
#define IMAGE_CPU_MASK       0x0700
#define IMAGE_CPU_ARM        0x0000
#define IMAGE_CHIP_MASK      0x7000
#define IMAGE_CHIP_RP2350    0x1000

// The bootrom looks for the first block in the first 4 KB
#define FIRST_BLOCK_WINDOW   4096
#define MAX_BLOCKS           8

#define SRAM_START           0x20000000u
#define SRAM_END             0x20082000u
#define FLASH_START          0x10000000u
#define FLASH_SIZE           (16u * 1024 * 1024)

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool fw_image_start_ok(const uint8_t *data, uint32_t len)
{
    if (len < 8)
        return false;
    uint32_t sp = rd32(data), reset = rd32(data + 4);
    return sp > SRAM_START && sp <= SRAM_END && (sp & 3) == 0 &&
           (reset & 1) && reset >= FLASH_START && reset < FLASH_START + FLASH_SIZE;
}

typedef struct {
    int32_t link;        // offset of the next block from this one
    bool image_type_ok;  // has an IMAGE_TYPE item for this chip
    bool hash;           // has a HASH_VALUE item
} block_t;

static fw_image_result_t parse_block(const uint8_t *image, uint32_t len, uint32_t at,
                                     block_t *b)
{
    uint32_t p = at + 4;  // past the start marker
    *b = (block_t){ 0 };

    for (;;) {
        if (p > len - 4)
            return FW_IMAGE_INCOMPLETE;
        uint32_t hdr = rd32(image + p);
        uint8_t type = hdr & 0xff;
        uint32_t size = (type & ITEM_2BS) ? (hdr >> 8) & 0xffff : (hdr >> 8) & 0xff;

        if (type == ITEM_LAST) {
            // Its size is the total of the items before it
            if (size != (p - (at + 4)) / 4)
                return FW_IMAGE_CORRUPTED;
            p += 4;
            break;
        }
        if (size == 0)
            return FW_IMAGE_CORRUPTED;
        if (type == ITEM_IMAGE_TYPE) {
            uint16_t flags = hdr >> 16;
            b->image_type_ok = (flags & IMAGE_TYPE_MASK) == IMAGE_TYPE_EXE &&
                               (flags & IMAGE_CPU_MASK) == IMAGE_CPU_ARM &&
                               (flags & IMAGE_CHIP_MASK) == IMAGE_CHIP_RP2350;
        } else if (type == ITEM_HASH_VALUE) {
            b->hash = true;
        }
        if (size > (len - p) / 4)
            return FW_IMAGE_INCOMPLETE;
        p += size * 4;
    }

    if (p > len - 8)
        return FW_IMAGE_INCOMPLETE;
    b->link = (int32_t)rd32(image + p);
    return rd32(image + p + 4) == BLOCK_MARKER_END ? FW_IMAGE_OK : FW_IMAGE_CORRUPTED;
}

fw_image_result_t fw_image_check(const uint8_t *image, uint32_t len)
{
    if (!fw_image_start_ok(image, len))
        return FW_IMAGE_INCOMPATIBLE;

    uint32_t first = 0;
    uint32_t window = len < FIRST_BLOCK_WINDOW ? len : FIRST_BLOCK_WINDOW;
    while (first + 4 <= window && rd32(image + first) != BLOCK_MARKER_START)
        first += 4;
    if (first + 4 > window)
        return FW_IMAGE_INCOMPATIBLE;

    bool type_ok = false, hash = false;
    uint32_t at = first;
    for (int n = 0; n < MAX_BLOCKS; n++) {
        block_t b;
        fw_image_result_t r = parse_block(image, len, at, &b);
        if (r != FW_IMAGE_OK)
            return r;
        type_ok |= b.image_type_ok;
        hash |= b.hash;

        int64_t next = (int64_t)at + b.link;
        if (next == first)
            // Loop closed: everything it covers has arrived. Without a hash
            // there is one block and no way to tell where the image ends.
            return type_ok && hash ? FW_IMAGE_OK : FW_IMAGE_INCOMPATIBLE;
        if (next < 0 || (next & 3))
            return FW_IMAGE_CORRUPTED;
        if (next > (int64_t)len - 4)
            return FW_IMAGE_INCOMPLETE;
        at = (uint32_t)next;
        if (rd32(image + at) != BLOCK_MARKER_START)
            return FW_IMAGE_CORRUPTED;
    }
    return FW_IMAGE_CORRUPTED;  // loop doesn't close
}
