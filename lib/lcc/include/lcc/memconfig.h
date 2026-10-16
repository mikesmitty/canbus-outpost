#ifndef LCC_MEMCONFIG_H
#define LCC_MEMCONFIG_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Memory Configuration protocol (datagram type 0x20): address spaces,
 * reads, writes, space information, configuration options, lock, update
 * complete, reboot, factory reset and firmware upgrade.
 *
 * Spaces are plain byte arrays in OpenLCB byte order, so tools read and
 * write exactly what is stored and multi-byte values stay big-endian.
 */

#define LCC_SPACE_CDI     0xFF
#define LCC_SPACE_ALL     0xFE
#define LCC_SPACE_CONFIG  0xFD
#define LCC_SPACE_FIRMWARE  0xEF

#define LCC_MEMCFG_CONTENT_TYPE  0x20

#ifndef LCC_MAX_MEMSPACES
#define LCC_MAX_MEMSPACES  4
#endif

typedef struct {
    uint8_t space;
    uint32_t size;
    const uint8_t *data;
    uint8_t *writable;   /* same buffer as data to allow writes, else NULL */
    /* Optional: called after a write lands in the buffer. */
    void (*written)(uint32_t address, uint32_t length);
} lcc_memspace_t;

/* The struct must stay valid; returns false when full. */
bool lcc_memconfig_add_space(const lcc_memspace_t *space);

/*
 * Firmware upgrade (Firmware Upgrade Standard). Freeze puts the node in
 * upgrade state, where the new image is written to space 0xEF in order from
 * address 0, and Unfreeze ends it. The application stores and checks the
 * image; the rest of the node keeps running meanwhile.
 *
 * Errors go back to the client in Datagram Rejected; these are the codes
 * the standard defines for firmware:
 */
#define LCC_ERROR_FIRMWARE_CHECKSUM      0x2088  /* temporary: try again */
#define LCC_ERROR_FIRMWARE_INCOMPATIBLE  0x1088  /* not for this hardware */
#define LCC_ERROR_FIRMWARE_CORRUPTED     0x1089

typedef struct {
    uint32_t max_size;  /* largest image accepted */
    /* Enter upgrade state. Return 0, or an error code to refuse. */
    uint16_t (*freeze)(void);
    /* Image data. Runs before the write is acknowledged, so the client
     * waits for it to finish; a write at address 0 starts over. Return 0
     * or an error code. */
    uint16_t (*write)(uint32_t address, const uint8_t *data, uint32_t len);
    /* End of the upgrade, after a failed transfer too. Return 0 if the
     * image is complete and valid: the reboot hook then runs to start it.
     * Otherwise return an error code for the client; the node carries on
     * with its current firmware. */
    uint16_t (*unfreeze)(void);
} lcc_firmware_upgrade_t;

/* Offer firmware upgrades. The struct must stay valid. */
bool lcc_memconfig_enable_firmware_upgrade(const lcc_firmware_upgrade_t *fw);

/* True in upgrade state: from Freeze until Unfreeze. */
bool lcc_firmware_upgrade_active(void);

#endif /* LCC_MEMCONFIG_H */
