#ifndef LCC_MEMCONFIG_H
#define LCC_MEMCONFIG_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Memory Configuration protocol (datagram type 0x20): address spaces,
 * reads, writes, space information, configuration options, lock, update
 * complete, reboot and factory reset.
 *
 * Spaces are plain byte arrays in OpenLCB byte order, so tools read and
 * write exactly what is stored and multi-byte values stay big-endian.
 */

#define LCC_SPACE_CDI     0xFF
#define LCC_SPACE_ALL     0xFE
#define LCC_SPACE_CONFIG  0xFD

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

#endif /* LCC_MEMCONFIG_H */
