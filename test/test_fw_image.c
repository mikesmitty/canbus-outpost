// Checks fw_image.c against images laid out like the firmware build: a
// vector table, a block near the start and a hash block at the end. Also
// runs against build/canbus-outpost.bin when it has been built.
#include <stdio.h>
#include <string.h>
#include "fw_image.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t img[0x3000];

static void wr32(uint32_t at, uint32_t v)
{
    img[at] = v & 0xff;
    img[at + 1] = (v >> 8) & 0xff;
    img[at + 2] = (v >> 16) & 0xff;
    img[at + 3] = (v >> 24) & 0xff;
}

#define START_BLOCK  0x138
#define END_BLOCK    0x2f00
#define IMAGE_LEN    (END_BLOCK + 23 * 4)   // ends with the end block
#define TYPE_ITEM(f) (0x00000142u | (uint32_t)(f) << 16)
#define RP2350_ARM   0x9021                 // EXE, secure, Arm, RP2350, TBYB

// Same items as the real build's blocks (see the comments in fw_image.h)
static void build(uint16_t type_flags)
{
    memset(img, 0xA5, sizeof(img));
    wr32(0, 0x20082000);  // initial stack pointer
    wr32(4, 0x1000015d);  // reset handler

    uint32_t p = START_BLOCK;
    wr32(p, 0xffffded3);
    wr32(p + 4, TYPE_ITEM(type_flags));
    wr32(p + 8, 0x000001ff);                 // LAST, 1 word before it
    wr32(p + 12, END_BLOCK - START_BLOCK);   // link
    wr32(p + 16, 0xab123579);

    p = END_BLOCK;
    wr32(p, 0xffffded3);
    wr32(p + 4, TYPE_ITEM(type_flags));
    wr32(p + 8, 0x02000706);                 // LOAD_MAP, 7 words
    wr32(p + 36, 0x01000247);                // HASH_DEF, 2 words
    wr32(p + 44, 0x0000094b);                // HASH_VALUE, 9 words
    wr32(p + 80, 0x000013ff);                // LAST, 19 words before it
    wr32(p + 84, (uint32_t)(START_BLOCK - END_BLOCK));
    wr32(p + 88, 0xab123579);
}

static void test_synthetic(void)
{
    build(RP2350_ARM);
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_OK, "good image rejected");
    CHECK(fw_image_check(img, IMAGE_LEN + 100) == FW_IMAGE_OK, "trailing bytes rejected");

    // Transfers that stopped early
    CHECK(fw_image_check(img, IMAGE_LEN - 1) == FW_IMAGE_INCOMPLETE, "missing last byte");
    CHECK(fw_image_check(img, END_BLOCK + 40) == FW_IMAGE_INCOMPLETE, "cut inside the end block");
    CHECK(fw_image_check(img, 0x1000) == FW_IMAGE_INCOMPLETE, "cut before the end block");
    CHECK(fw_image_check(img, START_BLOCK + 10) == FW_IMAGE_INCOMPLETE, "cut inside the first block");

    // Not an image for this board
    static const uint8_t uf2[16] = { 'U', 'F', '2', '\n', 0x57, 0x51, 0x5d, 0x9e };
    CHECK(!fw_image_start_ok(uf2, sizeof(uf2)), "UF2 file taken for an image");
    memcpy(img, uf2, sizeof(uf2));
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_INCOMPATIBLE, "UF2 file accepted");
    build(0x0021);  // RP2040
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_INCOMPATIBLE, "RP2040 image accepted");
    build(0x9121);  // RISC-V
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_INCOMPATIBLE, "RISC-V image accepted");

    // Built without pico_hash_binary: one block that links to itself
    build(RP2350_ARM);
    wr32(START_BLOCK + 12, 0);
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_INCOMPATIBLE, "image without a hash accepted");

    // No metadata in the first 4 KB
    build(RP2350_ARM);
    wr32(START_BLOCK, 0);
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_INCOMPATIBLE, "image without a block accepted");

    // Damaged metadata
    build(RP2350_ARM);
    wr32(END_BLOCK + 88, 0);
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_CORRUPTED, "bad end marker");
    build(RP2350_ARM);
    wr32(END_BLOCK + 80, 0x000012ff);
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_CORRUPTED, "wrong LAST size");
    build(RP2350_ARM);
    wr32(START_BLOCK + 12, END_BLOCK - START_BLOCK + 4);
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_CORRUPTED, "link to no block");
    build(RP2350_ARM);
    wr32(START_BLOCK + 12, 0x01000000);  // far outside the data: must not be read
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_INCOMPLETE, "link past the data");
    build(RP2350_ARM);
    wr32(END_BLOCK + 84, 0);  // end block links to itself: loop never closes
    CHECK(fw_image_check(img, IMAGE_LEN) == FW_IMAGE_CORRUPTED, "open loop");
}

static void test_build_output(void)
{
    FILE *f = fopen("../build/canbus-outpost.bin", "rb");
    if (!f) {
        printf("fw image: no build/canbus-outpost.bin, skipped the real image\n");
        return;
    }
    static uint8_t bin[2 * 1024 * 1024];
    uint32_t len = (uint32_t)fread(bin, 1, sizeof(bin), f);
    fclose(f);

    CHECK(fw_image_check(bin, len) == FW_IMAGE_OK, "build output rejected (%u bytes)", len);
    CHECK(fw_image_check(bin, len - 4) == FW_IMAGE_INCOMPLETE, "short build output accepted");
    CHECK(fw_image_check(bin, len / 2) == FW_IMAGE_INCOMPLETE, "half the build output accepted");
}

int main(void)
{
    test_synthetic();
    test_build_output();
    printf("fw image: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
