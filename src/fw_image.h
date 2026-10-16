#ifndef FW_IMAGE_H
#define FW_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

// Checks a firmware image received for an update before it is started.
//
// RP2350 images carry metadata blocks (RP2350 datasheet section 5.9,
// "picobin") linked in a loop: one near the start and, in builds made with
// pico_hash_binary, one at the end holding the image's SHA-256, which the
// bootrom verifies before running it. Following the loop shows whether the
// whole image arrived; the bootrom's hash check catches anything else.

typedef enum {
    FW_IMAGE_OK,
    FW_IMAGE_INCOMPATIBLE,  // not an RP2350 Arm executable with a hash
    FW_IMAGE_INCOMPLETE,    // the metadata extends past the data received
    FW_IMAGE_CORRUPTED,     // the metadata is damaged
} fw_image_result_t;

// The image's first bytes (8 or more): an Arm vector table for this chip.
bool fw_image_start_ok(const uint8_t *data, uint32_t len);

// The whole image, len bytes from its start.
fw_image_result_t fw_image_check(const uint8_t *image, uint32_t len);

#endif // FW_IMAGE_H
