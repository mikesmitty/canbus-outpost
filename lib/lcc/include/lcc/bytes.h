#ifndef LCC_BYTES_H
#define LCC_BYTES_H

#include <stdint.h>

/* OpenLCB sends every multi-byte value most significant byte first. */

static inline uint16_t lcc_get_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t lcc_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint64_t lcc_get_be48(const uint8_t *p)
{
    return ((uint64_t)lcc_get_be16(p) << 32) | lcc_get_be32(p + 2);
}

static inline uint64_t lcc_get_be64(const uint8_t *p)
{
    return ((uint64_t)lcc_get_be32(p) << 32) | lcc_get_be32(p + 4);
}

static inline void lcc_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void lcc_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void lcc_put_be48(uint8_t *p, uint64_t v)
{
    lcc_put_be16(p, (uint16_t)(v >> 32));
    lcc_put_be32(p + 2, (uint32_t)v);
}

static inline void lcc_put_be64(uint8_t *p, uint64_t v)
{
    lcc_put_be32(p, (uint32_t)(v >> 32));
    lcc_put_be32(p + 4, (uint32_t)v);
}

#endif /* LCC_BYTES_H */
