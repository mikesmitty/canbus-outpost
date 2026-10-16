#include <string.h>
#include "internal.h"
#include "lcc/bytes.h"
#include "lcc/memconfig.h"

/* CAN Frame Transfer Standard: wait at least 200 ms after the CID frames
 * before claiming the alias. */
#define CID_WAIT_MS  200

/* Longest user string sent in Simple Node Information */
#define SNIP_USER_MAX  63

lcc_node_config_t lcc_config;
uint32_t lcc_now;

typedef enum {
    STATE_IDLE,       /* not started */
    STATE_CHECKING,   /* CID frames sent, waiting for objections */
    STATE_PERMITTED,  /* alias claimed, Initialization Complete sent */
} node_state_t;

static struct {
    node_state_t state;
    uint16_t alias;
    uint32_t lfsr1, lfsr2;     /* alias generator state */
    uint32_t cid_deadline;
    bool conflict;             /* someone used or defended our alias while checking */
} node;

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

void lcc_send_frame(const lcc_frame_t *frame)
{
    lcc_config.hooks.send(frame);
}

void lcc_send_global(uint16_t mti, const uint8_t *data, uint8_t len)
{
    lcc_frame_t f = { .id = lcc_message_id(mti, node.alias), .dlc = len };
    if (len)
        memcpy(f.data, data, len);
    lcc_send_frame(&f);
}

void lcc_send_addressed(uint16_t mti, uint16_t dst_alias,
                        const uint8_t *data, uint8_t len)
{
    uint8_t offset = 0;
    do {
        uint8_t chunk = (uint8_t)(len - offset > 6 ? 6 : len - offset);
        uint8_t flags = 0;
        if (offset > 0)
            flags |= LCC_ADDR_NOT_FIRST_FRAME;
        if (offset + chunk < len)
            flags |= LCC_ADDR_NOT_LAST_FRAME;

        lcc_frame_t f = { .id = lcc_message_id(mti, node.alias), .dlc = 2 + chunk };
        f.data[0] = flags | ((dst_alias >> 8) & 0x0F);
        f.data[1] = dst_alias & 0xFF;
        memcpy(&f.data[2], data + offset, chunk);
        lcc_send_frame(&f);
        offset += chunk;
    } while (offset < len);
}

static void send_control(uint16_t field, const uint8_t *data, uint8_t len)
{
    lcc_frame_t f = { .id = lcc_control_id(field, 0, node.alias), .dlc = len };
    if (len)
        memcpy(f.data, data, len);
    lcc_send_frame(&f);
}

static void node_id_bytes(uint8_t out[6])
{
    lcc_put_be48(out, lcc_config.node_id);
}

/* ------------------------------------------------------------------ */
/* Alias allocation                                                   */
/* ------------------------------------------------------------------ */

/* Pseudo-random alias sequence seeded from the node ID, per the CAN Frame
 * Transfer Technical Note. */
static void alias_seed(void)
{
    node.lfsr1 = (uint32_t)(lcc_config.node_id >> 24) & 0xFFFFFF;
    node.lfsr2 = (uint32_t)lcc_config.node_id & 0xFFFFFF;
}

static void alias_step(void)
{
    uint32_t t1 = ((node.lfsr1 << 9) | ((node.lfsr2 >> 15) & 0x1FF)) & 0xFFFFFF;
    uint32_t t2 = (node.lfsr2 << 9) & 0xFFFFFF;
    node.lfsr1 = node.lfsr1 + t1 + 0x1B0CA3;
    node.lfsr2 = node.lfsr2 + t2 + 0x7A4BA9;
    node.lfsr1 = (node.lfsr1 & 0xFFFFFF) + ((node.lfsr2 & 0xFF000000) >> 24);
    node.lfsr2 &= 0xFFFFFF;
}

static uint16_t alias_value(void)
{
    return (node.lfsr1 ^ node.lfsr2 ^ (node.lfsr1 >> 12) ^ (node.lfsr2 >> 12)) & 0xFFF;
}

static void begin_checking(uint32_t now)
{
    do {
        alias_step();
        node.alias = alias_value();
    } while (node.alias == 0);

    node.state = STATE_CHECKING;
    node.conflict = false;

    for (int seq = 7; seq >= 4; seq--) {
        uint16_t bits = (lcc_config.node_id >> ((seq - 4) * 12)) & 0xFFF;
        lcc_frame_t f = { .id = lcc_control_id(bits, (uint8_t)seq, node.alias), .dlc = 0 };
        lcc_send_frame(&f);
    }
    node.cid_deadline = now + CID_WAIT_MS;
}

static void claim_alias(void)
{
    uint8_t nid[6];
    node_id_bytes(nid);

    send_control(LCC_CONTROL_RID, NULL, 0);
    send_control(LCC_CONTROL_AMD, nid, 6);
    node.state = STATE_PERMITTED;

    lcc_send_global(LCC_MTI_INITIALIZATION_COMPLETE, nid, 6);
    if (lcc_config.hooks.initialized)
        lcc_config.hooks.initialized();
    lcc_events_identify_all();
}

/* Another node sent a frame with our alias as its source. */
static void alias_collision(const lcc_frame_t *f, uint32_t now)
{
    if (node.state == STATE_CHECKING) {
        node.conflict = true;
        return;
    }

    if (lcc_is_cid_frame(f->id)) {
        /* Someone is checking an alias we hold: defend it */
        send_control(LCC_CONTROL_RID, NULL, 0);
        return;
    }

    /* Two nodes are using the alias: release it and start over */
    uint8_t nid[6];
    node_id_bytes(nid);
    send_control(LCC_CONTROL_AMR, nid, 6);
    lcc_datagram_alias_lost();
    begin_checking(now);
}

/* ------------------------------------------------------------------ */
/* Basic protocols                                                    */
/* ------------------------------------------------------------------ */

static void send_verified(void)
{
    uint8_t nid[6];
    node_id_bytes(nid);
    lcc_send_global(LCC_MTI_VERIFIED_NODE_ID, nid, 6);
    if (lcc_config.hooks.identify_requested)
        lcc_config.hooks.identify_requested();
}

static void send_protocol_support(uint16_t dst)
{
    uint64_t p = LCC_PROTOCOL_EVENT_EXCHANGE | LCC_PROTOCOL_SNIP;
    if (lcc_datagram_has_handlers())
        p |= LCC_PROTOCOL_DATAGRAM;
    if (lcc_memconfig_has_spaces())
        p |= LCC_PROTOCOL_MEMORY_CONFIG;
    if (lcc_memconfig_has_space(LCC_SPACE_CDI))
        p |= LCC_PROTOCOL_CDI;

    uint8_t buf[6];
    lcc_put_be48(buf, p);
    lcc_send_addressed(LCC_MTI_PROTOCOL_SUPPORT_REPLY, dst, buf, 6);
}

static int append_string(uint8_t *buf, int pos, const char *s, size_t max)
{
    // strnlen is POSIX, not C11
    const char *end = s ? memchr(s, 0, max) : NULL;
    size_t n = !s ? 0 : end ? (size_t)(end - s) : max;
    if (n)
        memcpy(buf + pos, s, n);
    buf[pos + n] = 0;
    return pos + (int)n + 1;
}

static void send_snip(uint16_t dst)
{
    /* Manufacturer version 4 + four strings, user version 2 + two strings */
    uint8_t buf[1 + 41 + 41 + 21 + 21 + 1 + 64 + 64];
    int pos = 0;

    buf[pos++] = 4;
    pos = append_string(buf, pos, lcc_config.manufacturer, 40);
    pos = append_string(buf, pos, lcc_config.model, 40);
    pos = append_string(buf, pos, lcc_config.hardware_version, 20);
    pos = append_string(buf, pos, lcc_config.software_version, 20);
    buf[pos++] = 2;
    pos = append_string(buf, pos, lcc_config.user_name, SNIP_USER_MAX);
    pos = append_string(buf, pos, lcc_config.user_description, SNIP_USER_MAX);

    lcc_send_addressed(LCC_MTI_SNIP_REPLY, dst, buf, (uint8_t)pos);
    if (lcc_config.hooks.identify_requested)
        lcc_config.hooks.identify_requested();
}

static void reject_interaction(uint16_t dst, uint16_t mti)
{
    uint8_t buf[4];
    lcc_put_be16(buf, LCC_ERROR_UNIMPLEMENTED_MTI);
    lcc_put_be16(buf + 2, mti);
    lcc_send_addressed(LCC_MTI_OPTIONAL_INTERACTION_REJECTED, dst, buf, 4);
}

static void handle_message(const lcc_frame_t *f, uint32_t now)
{
    uint16_t mti = lcc_frame_mti(f->id);
    uint16_t src = lcc_frame_src(f->id);
    const uint8_t *data = f->data;
    uint8_t len = f->dlc;

    if (mti & LCC_MTI_ADDRESSED_BIT) {
        if (len < 2 || (lcc_get_be16(data) & 0xFFF) != node.alias)
            return;
        bool first_frame = !(data[0] & LCC_ADDR_NOT_FIRST_FRAME);
        data += 2;
        len -= 2;

        switch (mti) {
        case LCC_MTI_VERIFY_NODE_ID_ADDRESSED:
            send_verified();
            return;
        case LCC_MTI_PROTOCOL_SUPPORT_INQUIRY:
            send_protocol_support(src);
            return;
        case LCC_MTI_SNIP_REQUEST:
            send_snip(src);
            return;
        case LCC_MTI_EVENTS_IDENTIFY_ADDRESSED:
            lcc_events_identify_all();
            return;
        case LCC_MTI_DATAGRAM_OK:
        case LCC_MTI_DATAGRAM_REJECTED:
            lcc_datagram_message(mti, src, data, len, now);
            return;
        case LCC_MTI_OPTIONAL_INTERACTION_REJECTED:
        case LCC_MTI_TERMINATE_DUE_TO_ERROR:
            return;  /* never answer these */
        default:
            if (first_frame)
                reject_interaction(src, mti);
            return;
        }
    }

    switch (mti) {
    case LCC_MTI_VERIFY_NODE_ID_GLOBAL:
        if (len == 0 || (len >= 6 && lcc_get_be48(data) == lcc_config.node_id))
            send_verified();
        return;
    default:
        lcc_events_handle(mti, data, len);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void lcc_init(const lcc_node_config_t *config)
{
    lcc_config = *config;
    memset(&node, 0, sizeof(node));
    alias_seed();
    lcc_events_reset();
    lcc_datagram_reset();
    lcc_memconfig_reset();
}

void lcc_start(uint32_t now_ms)
{
    lcc_now = now_ms;
    if (node.state == STATE_IDLE)
        begin_checking(now_ms);
}

void lcc_frame_received(const lcc_frame_t *frame, uint32_t now_ms)
{
    lcc_now = now_ms;
    if (node.state == STATE_IDLE)
        return;

    bool control = lcc_is_control_frame(frame->id);

    if (lcc_frame_src(frame->id) == node.alias) {
        alias_collision(frame, now_ms);
        return;
    }

    if (node.state != STATE_PERMITTED)
        return;

    if (control) {
        if (lcc_control_field(frame->id) == LCC_CONTROL_AME &&
            (frame->dlc == 0 ||
             (frame->dlc >= 6 && lcc_get_be48(frame->data) == lcc_config.node_id))) {
            uint8_t nid[6];
            node_id_bytes(nid);
            send_control(LCC_CONTROL_AMD, nid, 6);
        }
        return;
    }

    switch (lcc_can_frame_type(frame->id)) {
    case LCC_FRAME_GLOBAL_ADDRESSED:
        handle_message(frame, now_ms);
        break;
    case LCC_FRAME_DATAGRAM_ONE:
    case LCC_FRAME_DATAGRAM_FIRST:
    case LCC_FRAME_DATAGRAM_MIDDLE:
    case LCC_FRAME_DATAGRAM_FINAL:
        if (lcc_datagram_dst(frame->id) == node.alias)
            lcc_datagram_frame(frame, now_ms);
        break;
    default:
        break;  /* streams are not supported */
    }
}

void lcc_run_timers(uint32_t now_ms)
{
    lcc_now = now_ms;
    if (node.state == STATE_CHECKING && lcc_time_reached(now_ms, node.cid_deadline)) {
        if (node.conflict)
            begin_checking(now_ms);
        else
            claim_alias();
    }
    if (node.state == STATE_PERMITTED) {
        lcc_datagram_run_timers(now_ms);
        lcc_memconfig_run_timers(now_ms);
    }
}

uint32_t lcc_ms_until_next_timer(uint32_t now_ms)
{
    uint32_t next = LCC_NO_TIMER;
    lcc_timer_min(&next, node.state == STATE_CHECKING, now_ms, node.cid_deadline);
    if (node.state == STATE_PERMITTED) {
        uint32_t d = lcc_datagram_next_timer(now_ms);
        uint32_t m = lcc_memconfig_next_timer(now_ms);
        if (d < next) next = d;
        if (m < next) next = m;
    }
    return next;
}

bool lcc_is_initialized(void)
{
    return node.state == STATE_PERMITTED;
}

uint16_t lcc_alias(void)
{
    return node.alias;
}

uint64_t lcc_node_id(void)
{
    return lcc_config.node_id;
}
