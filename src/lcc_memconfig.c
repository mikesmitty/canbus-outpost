#include <string.h>
#include "lcc_memconfig.h"
#include "cdi_data.h"
#include "servo.h"
#include "util/dbg.h"

/* ------------------------------------------------------------------ */
/* Helpers: decode space number and address from datagram bytes        */
/* ------------------------------------------------------------------ */

/*
 * For Read (0x40) and Write (0x00) commands, the space number is encoded:
 *   - If (cmd & 0x03) != 0: space = 0xFC + (cmd & 0x03)
 *     So 0x43/0x03 => 0xFF (CDI), 0x42/0x02 => 0xFE, 0x41/0x01 => 0xFD (config)
 *   - If (cmd & 0x03) == 0: space is in byte 6, address in bytes 2-5
 */
static uint8_t decode_space(uint8_t cmd, const uint8_t *buf, uint8_t len,
                            uint32_t *addr, uint8_t *data_offset)
{
    /* Address is always in bytes 2-5 (big-endian) */
    *addr = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16)
          | ((uint32_t)buf[4] << 8)  | buf[5];

    uint8_t space_bits = cmd & 0x03;
    if (space_bits != 0) {
        *data_offset = 6;
        return 0xFC + space_bits;
    } else {
        *data_offset = 7;
        return (len > 6) ? buf[6] : 0;
    }
}

/* ------------------------------------------------------------------ */
/* Get Address Space Information                                       */
/* ------------------------------------------------------------------ */

static void handle_get_space_info(lcc_node_t *node, uint16_t src_alias,
                                  const uint8_t *buf, uint8_t len)
{
    if (len < 3)
        return;

    uint8_t space = buf[2];
    uint8_t reply[8];
    reply[0] = LCC_MEMCONFIG_CMD;

    switch (space) {
    case LCC_SPACE_CDI:
        reply[1] = LCC_MEMCFG_SPACE_INFO_REPLY;
        reply[2] = space;
        /* Highest address = cdi_xml_len - 1 (big-endian uint32) */
        {
            uint32_t highest = (uint32_t)(cdi_xml_len - 1);
            reply[3] = (highest >> 24) & 0xFF;
            reply[4] = (highest >> 16) & 0xFF;
            reply[5] = (highest >> 8)  & 0xFF;
            reply[6] = highest & 0xFF;
        }
        reply[7] = 0x01; /* flags: present, read-only */
        lcc_send_datagram(node, src_alias, reply, 8);
        break;

    case LCC_SPACE_CONFIG:
        reply[1] = LCC_MEMCFG_SPACE_INFO_REPLY;
        reply[2] = space;
        {
            uint32_t highest = (uint32_t)(LCC_CONFIG_SPACE_SIZE - 1);
            reply[3] = (highest >> 24) & 0xFF;
            reply[4] = (highest >> 16) & 0xFF;
            reply[5] = (highest >> 8)  & 0xFF;
            reply[6] = highest & 0xFF;
        }
        reply[7] = 0x00; /* flags: present, read-write */
        lcc_send_datagram(node, src_alias, reply, 8);
        break;

    default:
        reply[1] = LCC_MEMCFG_SPACE_NOT_KNOWN;
        reply[2] = space;
        lcc_send_datagram(node, src_alias, reply, 3);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Read command                                                        */
/* ------------------------------------------------------------------ */

static void handle_read(lcc_node_t *node, uint16_t src_alias,
                        const uint8_t *buf, uint8_t len)
{
    if (len < 7)
        return;

    uint32_t addr;
    uint8_t data_offset;
    uint8_t space = decode_space(buf[1], buf, len, &addr, &data_offset);

    if (data_offset >= len)
        return;
    uint8_t count = buf[data_offset];
    if (count == 0)
        return;

    const uint8_t *src_data = NULL;
    uint32_t space_size = 0;

    switch (space) {
    case LCC_SPACE_CDI:
        src_data = (const uint8_t *)cdi_xml;
        space_size = (uint32_t)cdi_xml_len;
        break;
    case LCC_SPACE_CONFIG:
        src_data = (const uint8_t *)&node->config + LCC_CONFIG_SPACE_OFFSET;
        space_size = LCC_CONFIG_SPACE_SIZE;
        break;
    default:
        DBG("LCC memconfig: read from unknown space 0x%02X\n", space);
        return;
    }

    /* Clamp read to space bounds */
    if (addr >= space_size) {
        count = 0;
    } else if (addr + count > space_size) {
        count = (uint8_t)(space_size - addr);
    }

    /* Build read reply datagram: cmd(1) + subcmd(1) + addr(4) + [space(1)] + data */
    uint8_t reply[72];
    uint8_t rpos = 0;
    reply[rpos++] = LCC_MEMCONFIG_CMD;

    /* Reply sub-command mirrors the space encoding from the request */
    uint8_t space_bits = buf[1] & 0x03;
    reply[rpos++] = LCC_MEMCFG_READ_REPLY | space_bits;

    /* Address (big-endian) */
    reply[rpos++] = (addr >> 24) & 0xFF;
    reply[rpos++] = (addr >> 16) & 0xFF;
    reply[rpos++] = (addr >> 8)  & 0xFF;
    reply[rpos++] = addr & 0xFF;

    /* Explicit space byte if space_bits == 0 */
    if (space_bits == 0)
        reply[rpos++] = space;

    /* Copy data */
    if (count > 0 && count <= (uint8_t)(sizeof(reply) - rpos)) {
        memcpy(reply + rpos, src_data + addr, count);
        rpos += count;
    }

    lcc_send_datagram(node, src_alias, reply, rpos);
}

/* ------------------------------------------------------------------ */
/* Write command                                                       */
/* ------------------------------------------------------------------ */

static void handle_write(lcc_node_t *node, uint16_t src_alias,
                         const uint8_t *buf, uint8_t len)
{
    if (len < 7)
        return;

    uint32_t addr;
    uint8_t data_offset;
    uint8_t space = decode_space(buf[1], buf, len, &addr, &data_offset);

    if (space != LCC_SPACE_CONFIG) {
        DBG("LCC memconfig: write to read-only space 0x%02X\n", space);
        return;
    }

    if (data_offset >= len)
        return;

    uint8_t count = len - data_offset;
    const uint8_t *write_data = buf + data_offset;

    uint8_t *config_bytes = (uint8_t *)&node->config + LCC_CONFIG_SPACE_OFFSET;

    /* Bounds check */
    if (addr >= LCC_CONFIG_SPACE_SIZE)
        return;
    if (addr + count > LCC_CONFIG_SPACE_SIZE)
        count = (uint8_t)(LCC_CONFIG_SPACE_SIZE - addr);

    memcpy(config_bytes + addr, write_data, count);

    DBG("LCC memconfig: wrote %d bytes at offset %lu\n", count, (unsigned long)addr);

    /* Check if a servo was just disabled — cut its PWM */
    for (int i = 0; i < LCC_NUM_SERVOS; i++) {
        if (!node->config.servos[i].enabled)
            servo_disable(i);
    }
}

/* ------------------------------------------------------------------ */
/* Update Complete / Resets                                             */
/* ------------------------------------------------------------------ */

static void handle_update_complete(lcc_node_t *node)
{
    DBG("LCC memconfig: update complete, saving to flash\n");
    lcc_save_config(node);
}

/* ------------------------------------------------------------------ */
/* Main dispatcher                                                     */
/* ------------------------------------------------------------------ */

void lcc_memconfig_handle(lcc_node_t *node, uint16_t src_alias,
                          const uint8_t *buf, uint8_t len)
{
    if (len < 2)
        return;

    uint8_t cmd = buf[1];

    if (cmd == LCC_MEMCFG_GET_SPACE_INFO) {
        handle_get_space_info(node, src_alias, buf, len);
        return;
    }

    if (cmd == LCC_MEMCFG_UPDATE_COMPLETE) {
        handle_update_complete(node);
        return;
    }

    /* Read commands: 0x40-0x4F */
    if ((cmd & 0xF0) == 0x40) {
        handle_read(node, src_alias, buf, len);
        return;
    }

    /* Write commands: 0x00-0x0F */
    if ((cmd & 0xF0) == 0x00) {
        handle_write(node, src_alias, buf, len);
        return;
    }

    DBG("LCC memconfig: unhandled sub-command 0x%02X\n", cmd);
}
