#include "node_id.h"
#include <stdio.h>
#include "lcc/bytes.h"
#include "nv_storage.h"
#include "pico/unique_id.h"

#define IDENTITY_VERSION 1

uint64_t node_id_load(bool *assigned)
{
    uint8_t buf[6];
    size_t len;
    uint16_t version;

    if (nv_load(&nv_identity_region, buf, sizeof(buf), &len, &version) &&
        version == IDENTITY_VERSION && len == sizeof(buf)) {
        uint64_t id = lcc_get_be48(buf);
        if (id != 0) {
            *assigned = true;
            return id;
        }
    }

    // Stand-in: low 6 bytes of the flash chip's unique ID
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    *assigned = false;
    return lcc_get_be48(&board_id.id[2]);
}

bool node_id_store(uint64_t node_id)
{
    uint8_t buf[6];
    lcc_put_be48(buf, node_id);
    return nv_save(&nv_identity_region, buf, sizeof(buf), IDENTITY_VERSION);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool node_id_parse(const char *s, uint64_t *node_id)
{
    uint64_t id = 0;
    int digits = 0;

    for (; *s; s++) {
        if (*s == '.' || *s == ':' || *s == ' ')
            continue;
        int v = hex_value(*s);
        if (v < 0 || ++digits > 12)
            return false;
        id = (id << 4) | (uint64_t)v;
    }
    if (digits != 12 || id == 0)
        return false;
    *node_id = id;
    return true;
}

void node_id_format(uint64_t node_id, char *buf)
{
    uint8_t b[6];
    lcc_put_be48(b, node_id);
    snprintf(buf, 18, "%02X.%02X.%02X.%02X.%02X.%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}
