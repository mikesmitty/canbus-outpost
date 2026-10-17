#include <string.h>
#include "internal.h"
#include "lcc/bytes.h"
#include "lcc/datagram.h"
#include "lcc/memconfig.h"

/* Command byte (second datagram byte) */
#define CMD_WRITE             0x00  /* 0x00-0x03, space in the low bits */
#define CMD_READ              0x40  /* 0x40-0x43 */
#define CMD_READ_REPLY        0x50
#define CMD_READ_REPLY_FAIL   0x58
#define CMD_OPTIONS           0x80
#define CMD_OPTIONS_REPLY     0x82
#define CMD_SPACE_INFO        0x84
#define CMD_SPACE_INFO_REPLY  0x86  /* | 0x01 when the space is present */
#define CMD_LOCK              0x88
#define CMD_LOCK_REPLY        0x8A
#define CMD_UNFREEZE          0xA0
#define CMD_FREEZE            0xA1
#define CMD_UPDATE_COMPLETE   0xA8
#define CMD_REBOOT            0xA9
#define CMD_FACTORY_RESET     0xAA

#define ERROR_SPACE_UNKNOWN   (LCC_ERROR_INVALID_ARGS | 0x01)
#define ERROR_OUT_OF_BOUNDS   (LCC_ERROR_INVALID_ARGS | 0x02)
#define ERROR_READ_ONLY       (LCC_ERROR_INVALID_ARGS | 0x03)

/* Configuration options reply */
#define OPT_UNALIGNED_READS   0x4000
#define OPT_UNALIGNED_WRITES  0x2000
#define OPT_WRITE_1_2_4_64    0xF0
#define OPT_WRITE_ARBITRARY   0x02

#define SPACE_FLAG_READ_ONLY  0x01

#define MAX_READ          64
#define REBOOT_DELAY_MS   100  /* let our replies and the Alias Map Reset go out */

static const lcc_memspace_t *spaces[LCC_MAX_MEMSPACES];
static int space_count;
static bool handler_registered;
static uint64_t lock_holder;
static bool reboot_pending;
static uint32_t reboot_at;
static const lcc_firmware_upgrade_t *firmware;
static bool frozen;
static bool start_firmware;  /* Unfreeze accepted the image */
static uint16_t unfreeze_result;  /* the answer to the last Unfreeze */

static const lcc_memspace_t *find_space(uint8_t space)
{
    for (int i = 0; i < space_count; i++) {
        if (spaces[i]->space == space)
            return spaces[i];
    }
    return NULL;
}

/* Read and write commands: [0x20, cmd, addr(4), space if cmd & 3 == 0, ...] */
typedef struct {
    uint8_t space_bits;
    uint8_t space;
    uint32_t address;
    uint8_t header_len;
} rw_cmd_t;

static bool parse_rw(const uint8_t *d, uint8_t len, rw_cmd_t *c)
{
    if (len < 6)
        return false;
    c->space_bits = d[1] & 0x03;
    c->address = lcc_get_be32(d + 2);
    if (c->space_bits) {
        c->space = 0xFC + c->space_bits;
        c->header_len = 6;
    } else {
        if (len < 7)
            return false;
        c->space = d[6];
        c->header_len = 7;
    }
    return true;
}

static uint8_t reply_header(uint8_t *buf, uint8_t cmd, const rw_cmd_t *c)
{
    buf[0] = LCC_MEMCFG_CONTENT_TYPE;
    buf[1] = cmd | c->space_bits;
    lcc_put_be32(buf + 2, c->address);
    if (c->space_bits == 0) {
        buf[6] = c->space;
        return 7;
    }
    return 6;
}

static void schedule_reboot(void)
{
    if (!lcc_config.hooks.reboot)
        return;
    lcc_release_alias();
    reboot_pending = true;
    reboot_at = lcc_now + REBOOT_DELAY_MS;
}

/* Freeze and Unfreeze: [0x20, cmd, space] */
static uint16_t accept_freeze(uint8_t cmd, const uint8_t *d, uint8_t len)
{
    if (len < 3)
        return LCC_ERROR_INVALID_ARGS;
    if (!firmware || d[2] != LCC_SPACE_FIRMWARE)
        return ERROR_SPACE_UNKNOWN;

    if (cmd == CMD_FREEZE) {
        if (!frozen) {
            uint16_t code = firmware->freeze();
            if (code)
                return code;
            frozen = true;  /* before the OK, so a Protocol Support reply says so */
        }
        return 0;
    }

    /* A client that resends an Unfreeze (its answer was lost, or it was a
     * temporary error) gets the same answer, not an OK for a bad image */
    if (!frozen)
        return unfreeze_result;
    /* Clients unfreeze after a failed transfer too, to end the upgrade, so
     * a bad image ends it as well; only a good one is started */
    frozen = false;
    unfreeze_result = firmware->unfreeze();
    start_firmware = unfreeze_result == 0;
    return unfreeze_result;
}

/* Firmware data goes to the application before the OK, so the client
 * doesn't send more while it is stored and errors can be reported. */
static uint16_t accept_firmware_write(const rw_cmd_t *c, const uint8_t *d, uint8_t len)
{
    uint32_t count = len - c->header_len;
    if (!firmware || !frozen)
        return ERROR_SPACE_UNKNOWN;  /* the space exists only in upgrade state */
    if (c->address >= firmware->max_size || count > firmware->max_size - c->address)
        return ERROR_OUT_OF_BOUNDS;
    return firmware->write(c->address, d + c->header_len, count);
}

/* ------------------------------------------------------------------ */
/* Datagram handler                                                   */
/* ------------------------------------------------------------------ */

static uint16_t accept(uint16_t src, const uint8_t *d, uint8_t len, bool *reply_pending)
{
    (void)src;
    if (len < 2)
        return LCC_ERROR_INVALID_ARGS;

    uint8_t cmd = d[1];
    rw_cmd_t c;

    if ((cmd & 0xFC) == CMD_READ) {
        if (!parse_rw(d, len, &c) || len <= c.header_len)
            return LCC_ERROR_INVALID_ARGS;
        uint8_t count = d[c.header_len];
        if (count == 0 || count > MAX_READ)
            return LCC_ERROR_INVALID_ARGS;
        *reply_pending = true;  /* failures go in a read reply */
        return 0;
    }

    if ((cmd & 0xFC) == CMD_WRITE) {
        if (!parse_rw(d, len, &c) || len <= c.header_len)
            return LCC_ERROR_INVALID_ARGS;
        if (c.space == LCC_SPACE_FIRMWARE)
            return accept_firmware_write(&c, d, len);
        const lcc_memspace_t *sp = find_space(c.space);
        uint32_t count = len - c.header_len;
        if (!sp)
            return ERROR_SPACE_UNKNOWN;
        if (!sp->writable)
            return ERROR_READ_ONLY;
        if (c.address >= sp->size || count > sp->size - c.address)
            return ERROR_OUT_OF_BOUNDS;
        return 0;  /* Datagram Received OK alone means the write is done */
    }

    switch (cmd) {
    case CMD_OPTIONS:
        *reply_pending = true;
        return 0;
    case CMD_SPACE_INFO:
        if (len < 3)
            return LCC_ERROR_INVALID_ARGS;
        *reply_pending = true;
        return 0;
    case CMD_LOCK:
        if (len < 8)
            return LCC_ERROR_INVALID_ARGS;
        *reply_pending = true;
        return 0;
    case CMD_UPDATE_COMPLETE:
    case CMD_REBOOT:
        return 0;
    case CMD_FREEZE:
    case CMD_UNFREEZE:
        return accept_freeze(cmd, d, len);
    case CMD_FACTORY_RESET:
        /* Must carry this node's ID as confirmation */
        if (len < 8 || lcc_get_be48(d + 2) != lcc_config.node_id)
            return LCC_ERROR_INVALID_ARGS;
        return 0;
    default:
        return LCC_ERROR_UNIMPLEMENTED_SUBCMD;
    }
}

static void do_read(uint16_t src, const uint8_t *d, uint8_t len)
{
    rw_cmd_t c;
    if (!parse_rw(d, len, &c))
        return;  /* accept() already rejected it */
    uint8_t count = d[c.header_len];
    const lcc_memspace_t *sp = find_space(c.space);

    uint8_t reply[7 + MAX_READ];
    uint16_t error = 0;
    if (!sp)
        error = ERROR_SPACE_UNKNOWN;
    else if (c.address >= sp->size)
        error = ERROR_OUT_OF_BOUNDS;

    if (error) {
        uint8_t n = reply_header(reply, CMD_READ_REPLY_FAIL, &c);
        lcc_put_be16(reply + n, error);
        lcc_datagram_send(src, reply, n + 2);
        return;
    }

    uint8_t n = reply_header(reply, CMD_READ_REPLY, &c);
    if (count > sp->size - c.address)
        count = (uint8_t)(sp->size - c.address);
    memcpy(reply + n, sp->data + c.address, count);
    lcc_datagram_send(src, reply, n + count);
}

static void do_write(const uint8_t *d, uint8_t len)
{
    rw_cmd_t c;
    if (!parse_rw(d, len, &c))
        return;  /* accept() already rejected it */
    const lcc_memspace_t *sp = find_space(c.space);
    uint32_t count = len - c.header_len;
    if (!sp)
        return;  /* firmware: stored in accept() */

    memcpy(sp->writable + c.address, d + c.header_len, count);
    if (sp->written)
        sp->written(c.address, count);
}

static void do_options(uint16_t src)
{
    uint8_t highest = 0, lowest = 0xFF;
    for (int i = 0; i < space_count; i++) {
        if (spaces[i]->space > highest) highest = spaces[i]->space;
        if (spaces[i]->space < lowest) lowest = spaces[i]->space;
    }
    if (firmware && LCC_SPACE_FIRMWARE < lowest)
        lowest = LCC_SPACE_FIRMWARE;

    uint8_t reply[7];
    reply[0] = LCC_MEMCFG_CONTENT_TYPE;
    reply[1] = CMD_OPTIONS_REPLY;
    lcc_put_be16(reply + 2, OPT_UNALIGNED_READS | OPT_UNALIGNED_WRITES);
    reply[4] = OPT_WRITE_1_2_4_64 | OPT_WRITE_ARBITRARY;
    reply[5] = highest;
    reply[6] = lowest;
    lcc_datagram_send(src, reply, sizeof(reply));
}

static void do_space_info(uint16_t src, uint8_t space)
{
    const lcc_memspace_t *sp = find_space(space);
    uint8_t reply[8];
    reply[0] = LCC_MEMCFG_CONTENT_TYPE;
    reply[2] = space;

    if (space == LCC_SPACE_FIRMWARE && firmware && frozen) {
        reply[1] = CMD_SPACE_INFO_REPLY | 0x01;
        lcc_put_be32(reply + 3, firmware->max_size - 1);
        reply[7] = 0;
        lcc_datagram_send(src, reply, sizeof(reply));
        return;
    }

    if (!sp) {
        reply[1] = CMD_SPACE_INFO_REPLY;
        lcc_datagram_send(src, reply, 3);
        return;
    }

    reply[1] = CMD_SPACE_INFO_REPLY | 0x01;
    lcc_put_be32(reply + 3, sp->size ? sp->size - 1 : 0);
    reply[7] = sp->writable ? 0 : SPACE_FLAG_READ_ONLY;
    lcc_datagram_send(src, reply, sizeof(reply));
}

static void do_lock(uint16_t src, uint64_t requester)
{
    if (requester == 0)
        lock_holder = 0;           /* release */
    else if (lock_holder == 0)
        lock_holder = requester;

    uint8_t reply[8];
    reply[0] = LCC_MEMCFG_CONTENT_TYPE;
    reply[1] = CMD_LOCK_REPLY;
    lcc_put_be48(reply + 2, lock_holder);
    lcc_datagram_send(src, reply, sizeof(reply));
}

static void process(uint16_t src, const uint8_t *d, uint8_t len)
{
    uint8_t cmd = d[1];

    if ((cmd & 0xFC) == CMD_READ) {
        do_read(src, d, len);
        return;
    }
    if ((cmd & 0xFC) == CMD_WRITE) {
        do_write(d, len);
        return;
    }

    switch (cmd) {
    case CMD_OPTIONS:
        do_options(src);
        break;
    case CMD_SPACE_INFO:
        do_space_info(src, d[2]);
        break;
    case CMD_LOCK:
        do_lock(src, lcc_get_be48(d + 2));
        break;
    case CMD_UPDATE_COMPLETE:
        if (lcc_config.hooks.config_update_complete)
            lcc_config.hooks.config_update_complete();
        break;
    case CMD_REBOOT:
        schedule_reboot();
        break;
    case CMD_FREEZE:
        /* The standard has the node acknowledge the change of state as if
         * it had restarted */
        lcc_send_initialization_complete();
        break;
    case CMD_UNFREEZE:
        if (start_firmware) {
            start_firmware = false;
            schedule_reboot();  /* the new firmware announces itself */
        }
        break;
    case CMD_FACTORY_RESET:
        if (lcc_config.hooks.factory_reset)
            lcc_config.hooks.factory_reset();
        schedule_reboot();
        break;
    default:
        break;
    }
}

static const lcc_datagram_handler_t handler = {
    .content_type = LCC_MEMCFG_CONTENT_TYPE,
    .accept = accept,
    .process = process,
};

/* ------------------------------------------------------------------ */
/* Public API and internal hooks                                      */
/* ------------------------------------------------------------------ */

static bool register_handler(void)
{
    if (!handler_registered) {
        if (!lcc_datagram_register(&handler))
            return false;
        handler_registered = true;
    }
    return true;
}

bool lcc_memconfig_add_space(const lcc_memspace_t *space)
{
    if (space_count >= LCC_MAX_MEMSPACES || !register_handler())
        return false;
    spaces[space_count++] = space;
    return true;
}

bool lcc_memconfig_enable_firmware_upgrade(const lcc_firmware_upgrade_t *fw)
{
    if (!register_handler())
        return false;
    firmware = fw;
    return true;
}

bool lcc_firmware_upgrade_active(void)
{
    return frozen;
}

bool lcc_memconfig_has_firmware_upgrade(void)
{
    return firmware != NULL;
}

void lcc_memconfig_reset(void)
{
    space_count = 0;
    handler_registered = false;
    lock_holder = 0;
    reboot_pending = false;
    firmware = NULL;
    frozen = false;
    start_firmware = false;
    unfreeze_result = 0;
}

void lcc_reboot(uint32_t now_ms)
{
    lcc_now = now_ms;
    schedule_reboot();
}

void lcc_memconfig_run_timers(uint32_t now)
{
    if (reboot_pending && lcc_time_reached(now, reboot_at)) {
        reboot_pending = false;
        lcc_config.hooks.reboot();
    }
}

uint32_t lcc_memconfig_next_timer(uint32_t now)
{
    uint32_t next = LCC_NO_TIMER;
    lcc_timer_min(&next, reboot_pending, now, reboot_at);
    return next;
}

bool lcc_memconfig_has_space(uint8_t space)
{
    return find_space(space) != NULL;
}

bool lcc_memconfig_has_spaces(void)
{
    return space_count > 0 || firmware != NULL;
}
