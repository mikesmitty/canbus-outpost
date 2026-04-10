#include <string.h>
#include "lcc.h"
#include "lcc_memconfig.h"
#include "identify_led.h"
#include "servo.h"
#include "nv_storage.h"
#include "util/dbg.h"

#define CAN_QUEUE_LEN  16
#define GC_QUEUE_LEN   16

/* ------------------------------------------------------------------ */
/* Alias generation (LFSR, per OpenMRN AliasAllocator)                */
/* ------------------------------------------------------------------ */

static uint16_t alias_seed(uint64_t node_id)
{
    uint16_t a = (node_id >> 36) & 0xFFF;
    uint16_t b = (node_id >> 24) & 0xFFF;
    uint16_t c = (node_id >> 12) & 0xFFF;
    uint16_t d = (node_id >>  0) & 0xFFF;
    return (a ^ b ^ c ^ d) & 0xFFF;
}

static uint16_t alias_next(uint16_t alias)
{
    /* 12-bit LFSR step from OpenMRN */
    uint16_t a = ((alias >> 0) & 1) ^ ((alias >> 2) & 1)
               ^ ((alias >> 3) & 1) ^ ((alias >> 11) & 1);
    alias = (alias >> 1) | (a << 11);
    if (alias == 0)
        alias = 1;
    return alias & 0xFFF;
}

/* ------------------------------------------------------------------ */
/* Frame send helpers                                                 */
/* ------------------------------------------------------------------ */

static void lcc_send(lcc_node_t *node, const lcc_frame_t *frame)
{
    xQueueSend(node->can_tx_queue, frame, pdMS_TO_TICKS(50));
    xQueueSend(node->gc_tx_queue, frame, 0);
}

static void lcc_send_global(lcc_node_t *node, uint16_t mti,
                            const uint8_t *data, uint8_t dlc)
{
    lcc_frame_t f;
    f.id = lcc_can_id(mti, node->alias);
    f.dlc = dlc;
    if (data && dlc > 0)
        memcpy(f.data, data, dlc);
    lcc_send(node, &f);
}

static void lcc_send_addressed(lcc_node_t *node, uint16_t mti,
                               uint16_t dst_alias,
                               const uint8_t *payload, uint8_t payload_len)
{
    /*
     * Addressed messages: first 2 bytes are destination alias with
     * continuation flags in upper nibble. Max 6 payload bytes per frame.
     */
    uint8_t offset = 0;
    bool first = true;

    while (offset < payload_len || first) {
        lcc_frame_t f;
        f.id = lcc_can_id(mti, node->alias);

        uint8_t remaining = payload_len - offset;
        uint8_t chunk = (remaining > 6) ? 6 : remaining;
        bool last = (offset + chunk >= payload_len);

        uint8_t flags = 0;
        if (!first) flags |= LCC_NOT_FIRST_FRAME;
        if (!last)  flags |= LCC_NOT_LAST_FRAME;

        f.data[0] = ((dst_alias >> 8) & 0x0F) | flags;
        f.data[1] = dst_alias & 0xFF;
        if (chunk > 0)
            memcpy(&f.data[2], payload + offset, chunk);
        f.dlc = 2 + chunk;

        lcc_send(node, &f);
        offset += chunk;
        first = false;
    }
}

static void lcc_send_control(lcc_node_t *node, uint16_t field,
                             const uint8_t *data, uint8_t dlc)
{
    lcc_frame_t f;
    f.id = lcc_control_id(node->alias, field, 0);
    f.dlc = dlc;
    if (data && dlc > 0)
        memcpy(f.data, data, dlc);
    lcc_send(node, &f);
}

/* ------------------------------------------------------------------ */
/* Alias allocation                                                   */
/* ------------------------------------------------------------------ */

static void lcc_send_cid(lcc_node_t *node)
{
    /* 4 CID frames: sequence 7,6,5,4 each carrying 12 bits of node_id */
    for (int seq = 7; seq >= 4; seq--) {
        int shift = (seq - 4) * 12;
        uint16_t field = (node->node_id >> shift) & 0xFFF;

        lcc_frame_t f;
        f.id = lcc_control_id(node->alias, field, seq);
        f.dlc = 0;
        lcc_send(node, &f);
    }
}

static bool lcc_alias_allocate(lcc_node_t *node)
{
    node->alias = alias_seed(node->node_id);
    if (node->alias == 0)
        node->alias = 1;

    for (int attempt = 0; attempt < 10; attempt++) {
        DBG("LCC: trying alias 0x%03X\n", node->alias);
        node->state = LCC_STATE_ALIAS_CID;

        lcc_send_cid(node);

        /* Wait 200ms for conflicts */
        node->state = LCC_STATE_ALIAS_WAIT;
        lcc_frame_t f;
        bool conflict = false;
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);

        while (xTaskGetTickCount() < deadline) {
            TickType_t remaining = deadline - xTaskGetTickCount();
            if (xQueueReceive(node->can_rx_queue, &f, remaining)) {
                /* Check for RID with our alias — conflict */
                if (lcc_is_control_frame(f.id) &&
                    lcc_get_src(f.id) == node->alias) {
                    conflict = true;
                    break;
                }
                /* Check for CID with our alias — conflict */
                if (lcc_is_cid_frame(f.id) &&
                    lcc_get_src(f.id) == node->alias) {
                    conflict = true;
                    break;
                }
            }
        }

        if (!conflict) {
            /* Claim alias: RID + AMD + Init Complete */
            lcc_send_control(node, LCC_CONTROL_RID, NULL, 0);

            uint8_t nid_buf[6];
            lcc_node_id_to_buf(node->node_id, nid_buf);
            lcc_send_control(node, LCC_CONTROL_AMD, nid_buf, 6);

            lcc_send_global(node, LCC_MTI_INITIALIZATION_COMPLETE, nid_buf, 6);

            node->state = LCC_STATE_INITIALIZED;
            DBG("LCC: initialized with alias 0x%03X\n", node->alias);
            return true;
        }

        DBG("LCC: alias 0x%03X conflict, retrying\n", node->alias);
        node->alias = alias_next(node->alias);
    }

    DBG("LCC: failed to allocate alias after 10 attempts\n");
    return false;
}

/* ------------------------------------------------------------------ */
/* Protocol handlers                                                  */
/* ------------------------------------------------------------------ */

static void handle_verify_node_id(lcc_node_t *node, const lcc_frame_t *f)
{
    uint16_t mti = lcc_get_mti(f->id);

    /* If addressed, check destination */
    if (mti == LCC_MTI_VERIFY_NODE_ID_ADDRESSED) {
        if (f->dlc < 2)
            return;
        uint16_t dst = ((f->data[0] & 0x0F) << 8) | f->data[1];
        if (dst != node->alias)
            return;
    }

    /* If global with node_id payload, check it matches */
    if (mti == LCC_MTI_VERIFY_NODE_ID_GLOBAL && f->dlc == 6) {
        uint64_t target = 0;
        for (int i = 0; i < 6; i++)
            target = (target << 8) | f->data[i];
        if (target != node->node_id)
            return;
    }

    uint8_t nid_buf[6];
    lcc_node_id_to_buf(node->node_id, nid_buf);
    lcc_send_global(node, LCC_MTI_VERIFIED_NODE_ID, nid_buf, 6);
    identify_led_trigger(&node->config.identify);
}

static void handle_pip(lcc_node_t *node, const lcc_frame_t *f)
{
    if (f->dlc < 2)
        return;
    uint16_t dst = ((f->data[0] & 0x0F) << 8) | f->data[1];
    if (dst != node->alias)
        return;

    uint64_t protocols = LCC_PIP_SIMPLE_PROTOCOL
                       | LCC_PIP_DATAGRAM
                       | LCC_PIP_MEMORY_CONFIG
                       | LCC_PIP_CDI
                       | LCC_PIP_EVENT_EXCHANGE
                       | LCC_PIP_IDENTIFICATION
                       | LCC_PIP_SIMPLE_NODE_INFO;

    uint8_t pip[6];
    pip[0] = (protocols >> 40) & 0xFF;
    pip[1] = (protocols >> 32) & 0xFF;
    pip[2] = (protocols >> 24) & 0xFF;
    pip[3] = (protocols >> 16) & 0xFF;
    pip[4] = (protocols >>  8) & 0xFF;
    pip[5] = (protocols >>  0) & 0xFF;

    uint16_t src = lcc_get_src(f->id);
    lcc_send_addressed(node, LCC_MTI_PROTOCOL_SUPPORT_REPLY, src, pip, 6);
}

static void handle_snip(lcc_node_t *node, const lcc_frame_t *f)
{
    if (f->dlc < 2)
        return;
    uint16_t dst = ((f->data[0] & 0x0F) << 8) | f->data[1];
    if (dst != node->alias)
        return;

    /*
     * SNIP version 4 format:
     * [4] mfg_name \0 model \0 hw_version \0 sw_version \0
     * [2] user_name \0 user_desc \0
     */
    uint8_t snip[128];
    int pos = 0;

    snip[pos++] = 4; /* manufacturer info version */
    const char *strs[] = {
        LCC_SNIP_MFG_NAME, LCC_SNIP_MODEL_NAME,
        LCC_SNIP_HW_VERSION, LCC_SNIP_SW_VERSION
    };
    for (int i = 0; i < 4; i++) {
        int len = strlen(strs[i]);
        memcpy(&snip[pos], strs[i], len + 1);
        pos += len + 1;
    }

    snip[pos++] = 2; /* user info version */
    /* User name from config (null-terminated) */
    {
        const char *uname = (const char *)node->config.user_name;
        int ulen = strlen(uname);
        if (ulen > 63) ulen = 63;
        if (ulen > 0) {
            memcpy(&snip[pos], uname, ulen);
            pos += ulen;
        }
        snip[pos++] = 0;
    }
    /* User description from config (null-terminated) */
    {
        const char *udesc = (const char *)node->config.user_desc;
        int ulen = strlen(udesc);
        if (ulen > 63) ulen = 63;
        if (ulen > 0) {
            memcpy(&snip[pos], udesc, ulen);
            pos += ulen;
        }
        snip[pos++] = 0;
    }

    uint16_t src = lcc_get_src(f->id);
    lcc_send_addressed(node, LCC_MTI_IDENT_INFO_REPLY, src, snip, pos);
    identify_led_trigger(&node->config.identify);
}

static uint64_t servo_event(lcc_node_t *node, int event_idx)
{
    int servo = event_idx / 2;
    bool is_close = (event_idx & 1);
    return is_close ? node->config.servos[servo].close_event
                    : node->config.servos[servo].throw_event;
}

static void send_consumer_identified(lcc_node_t *node, int event_idx)
{
    uint8_t buf[8];
    lcc_event_to_buf(servo_event(node, event_idx), buf);

    int servo = event_idx / 2;
    bool is_close = (event_idx & 1);
    bool active = (is_close == node->servo_state[servo]);

    uint16_t mti = active ? LCC_MTI_CONSUMER_IDENTIFIED_VALID
                          : LCC_MTI_CONSUMER_IDENTIFIED_INVALID;
    lcc_send_global(node, mti, buf, 8);
}

static void send_producer_identified(lcc_node_t *node, int servo, bool is_closed_fb)
{
    lcc_servo_config_t *sc = &node->config.servos[servo];
    uint64_t event_id = is_closed_fb ? sc->closed_feedback : sc->thrown_feedback;
    if (event_id == 0)
        return;

    uint8_t buf[8];
    lcc_event_to_buf(event_id, buf);

    /* Active if the current state matches this feedback direction */
    bool active;
    if (is_closed_fb)
        active = node->servo_state[servo] && !servo_is_moving(servo);
    else
        active = !node->servo_state[servo] && !servo_is_moving(servo);

    uint16_t mti = active ? LCC_MTI_PRODUCER_IDENTIFIED_VALID
                          : LCC_MTI_PRODUCER_IDENTIFIED_INVALID;
    lcc_send_global(node, mti, buf, 8);
}

static void handle_identify_events(lcc_node_t *node, const lcc_frame_t *f)
{
    uint16_t mti = lcc_get_mti(f->id);

    if (mti == LCC_MTI_EVENTS_IDENTIFY_ADDRESSED) {
        if (f->dlc < 2)
            return;
        uint16_t dst = ((f->data[0] & 0x0F) << 8) | f->data[1];
        if (dst != node->alias)
            return;
    }

    /* Consumer events (throw/close commands) */
    for (int i = 0; i < LCC_NUM_EVENTS; i++)
        send_consumer_identified(node, i);

    /* Producer events (thrown/closed feedback) */
    for (int i = 0; i < LCC_NUM_SERVOS; i++) {
        send_producer_identified(node, i, false); /* thrown feedback */
        send_producer_identified(node, i, true);  /* closed feedback */
    }
}

static void handle_producer_identify(lcc_node_t *node, const lcc_frame_t *f)
{
    if (f->dlc < 8)
        return;

    uint64_t event_id = lcc_event_from_buf(f->data);

    for (int i = 0; i < LCC_NUM_SERVOS; i++) {
        lcc_servo_config_t *sc = &node->config.servos[i];
        if (event_id == sc->thrown_feedback) {
            send_producer_identified(node, i, false);
            return;
        }
        if (event_id == sc->closed_feedback) {
            send_producer_identified(node, i, true);
            return;
        }
    }
}

static void handle_consumer_identify(lcc_node_t *node, const lcc_frame_t *f)
{
    if (f->dlc < 8)
        return;

    uint64_t event_id = lcc_event_from_buf(f->data);

    for (int i = 0; i < LCC_NUM_EVENTS; i++) {
        if (servo_event(node, i) == event_id) {
            send_consumer_identified(node, i);
            return;
        }
    }
}

static void handle_event_report(lcc_node_t *node, const lcc_frame_t *f)
{
    if (f->dlc < 8)
        return;

    uint64_t event_id = lcc_event_from_buf(f->data);

    for (int i = 0; i < LCC_NUM_SERVOS; i++) {
        lcc_servo_config_t *sc = &node->config.servos[i];
        if (!sc->enabled)
            continue;

        if (event_id == sc->throw_event) {
            uint16_t pos = sc->reversed ? sc->closed_position : sc->thrown_position;
            servo_set_target(i, pos, sc->throw_time, sc->hold_time);
            node->servo_state[i] = false;
            DBG("LCC: servo %d throw -> %u (time %u.%us)\n",
                i, pos, sc->throw_time / 10, sc->throw_time % 10);
            return;
        } else if (event_id == sc->close_event) {
            uint16_t pos = sc->reversed ? sc->thrown_position : sc->closed_position;
            servo_set_target(i, pos, sc->throw_time, sc->hold_time);
            node->servo_state[i] = true;
            DBG("LCC: servo %d close -> %u (time %u.%us)\n",
                i, pos, sc->throw_time / 10, sc->throw_time % 10);
            return;
        }
    }
}

static void handle_ame(lcc_node_t *node, const lcc_frame_t *f)
{
    /* If AME carries a node_id, only respond if it matches */
    if (f->dlc == 6) {
        uint64_t target = 0;
        for (int i = 0; i < 6; i++)
            target = (target << 8) | f->data[i];
        if (target != node->node_id)
            return;
    }

    uint8_t nid_buf[6];
    lcc_node_id_to_buf(node->node_id, nid_buf);
    lcc_send_control(node, LCC_CONTROL_AMD, nid_buf, 6);
}

void lcc_send_datagram_ok(lcc_node_t *node, uint16_t dst_alias)
{
    uint8_t data[2];
    data[0] = (dst_alias >> 8) & 0x0F;
    data[1] = dst_alias & 0xFF;

    lcc_frame_t f;
    f.id = lcc_can_id(LCC_MTI_DATAGRAM_OK, node->alias);
    f.dlc = 2;
    memcpy(f.data, data, 2);
    lcc_send(node, &f);
}

static void lcc_send_datagram_rejected(lcc_node_t *node, uint16_t dst_alias,
                                       uint16_t error_code)
{
    uint8_t data[4];
    data[0] = (dst_alias >> 8) & 0x0F;
    data[1] = dst_alias & 0xFF;
    data[2] = (error_code >> 8) & 0xFF;
    data[3] = error_code & 0xFF;

    lcc_frame_t f;
    f.id = lcc_can_id(LCC_MTI_DATAGRAM_REJECTED, node->alias);
    f.dlc = 4;
    memcpy(f.data, data, 4);
    lcc_send(node, &f);
}

static void handle_complete_datagram(lcc_node_t *node, uint16_t src_alias,
                                     const uint8_t *buf, uint8_t len)
{
    /* ACK the datagram first */
    lcc_send_datagram_ok(node, src_alias);

    if (len < 1)
        return;

    if (buf[0] == LCC_MEMCONFIG_CMD) {
        lcc_memconfig_handle(node, src_alias, buf, len);
    } else {
        /* Unknown datagram protocol */
        DBG("LCC: unknown datagram protocol 0x%02X\n", buf[0]);
    }
}

static void handle_datagram(lcc_node_t *node, const lcc_frame_t *f)
{
    uint8_t frame_type = lcc_get_can_frame_type(f->id);
    uint16_t dst = lcc_get_datagram_dst(f->id);
    uint16_t src = lcc_get_src(f->id);

    /* Ignore datagrams not addressed to us */
    if (dst != node->alias)
        return;

    lcc_datagram_rx_t *dg = &node->dg_rx;

    switch (frame_type) {
    case LCC_FRAME_DATAGRAM_ONE:
        /* Complete single-frame datagram */
        handle_complete_datagram(node, src, f->data, f->dlc);
        dg->active = false;
        break;

    case LCC_FRAME_DATAGRAM_FIRST:
        /* Start of multi-frame datagram */
        dg->src_alias = src;
        dg->len = 0;
        dg->active = true;
        if (f->dlc <= LCC_DATAGRAM_MAX) {
            memcpy(dg->buf, f->data, f->dlc);
            dg->len = f->dlc;
        }
        break;

    case LCC_FRAME_DATAGRAM_MIDDLE:
        if (!dg->active || dg->src_alias != src)
            break;
        if (dg->len + f->dlc > LCC_DATAGRAM_MAX) {
            /* Overflow — reject */
            lcc_send_datagram_rejected(node, src, 0x1000);
            dg->active = false;
            break;
        }
        memcpy(dg->buf + dg->len, f->data, f->dlc);
        dg->len += f->dlc;
        break;

    case LCC_FRAME_DATAGRAM_FINAL:
        if (!dg->active || dg->src_alias != src) {
            /* Got FINAL without FIRST — treat as single-frame */
            if (!dg->active) {
                handle_complete_datagram(node, src, f->data, f->dlc);
            }
            break;
        }
        if (dg->len + f->dlc > LCC_DATAGRAM_MAX) {
            lcc_send_datagram_rejected(node, src, 0x1000);
            dg->active = false;
            break;
        }
        memcpy(dg->buf + dg->len, f->data, f->dlc);
        dg->len += f->dlc;
        dg->active = false;
        handle_complete_datagram(node, src, dg->buf, dg->len);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Frame dispatcher                                                   */
/* ------------------------------------------------------------------ */

static void lcc_handle_frame(lcc_node_t *node, const lcc_frame_t *f)
{
    if (lcc_is_control_frame(f->id)) {
        uint16_t field = lcc_get_control_field(f->id);

        /* Defend our alias against CID */
        if (lcc_is_cid_frame(f->id) &&
            lcc_get_src(f->id) == node->alias) {
            lcc_send_control(node, LCC_CONTROL_RID, NULL, 0);
            return;
        }

        if (field == LCC_CONTROL_AME)
            handle_ame(node, f);

        return;
    }

    uint8_t frame_type = lcc_get_can_frame_type(f->id);

    switch (frame_type) {
    case LCC_FRAME_GLOBAL_ADDRESSED: {
        uint16_t mti = lcc_get_mti(f->id);

        switch (mti) {
        case LCC_MTI_VERIFY_NODE_ID_GLOBAL:
        case LCC_MTI_VERIFY_NODE_ID_ADDRESSED:
            handle_verify_node_id(node, f);
            break;
        case LCC_MTI_PROTOCOL_SUPPORT_INQUIRY:
            handle_pip(node, f);
            break;
        case LCC_MTI_IDENT_INFO_REQUEST:
            handle_snip(node, f);
            break;
        case LCC_MTI_EVENTS_IDENTIFY_GLOBAL:
        case LCC_MTI_EVENTS_IDENTIFY_ADDRESSED:
            handle_identify_events(node, f);
            break;
        case LCC_MTI_CONSUMER_IDENTIFY:
            handle_consumer_identify(node, f);
            break;
        case LCC_MTI_PRODUCER_IDENTIFY:
            handle_producer_identify(node, f);
            break;
        case LCC_MTI_EVENT_REPORT:
            handle_event_report(node, f);
            break;
        default:
            break;
        }
        break;
    }
    case LCC_FRAME_DATAGRAM_ONE:
    case LCC_FRAME_DATAGRAM_FIRST:
    case LCC_FRAME_DATAGRAM_MIDDLE:
    case LCC_FRAME_DATAGRAM_FINAL:
        handle_datagram(node, f);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void lcc_config_defaults(lcc_config_t *config, uint64_t node_id)
{
    memset(config, 0, sizeof(*config));
    config->magic = LCC_CONFIG_MAGIC;
    config->identify.enabled = 0;
    config->identify.timeout_minutes = LCC_IDENTIFY_TIMEOUT_5_MIN;

    for (int i = 0; i < LCC_NUM_SERVOS; i++) {
        lcc_servo_config_t *sc = &config->servos[i];
        sc->throw_event    = (node_id << 16) | (i * 4);
        sc->close_event    = (node_id << 16) | (i * 4 + 1);
        sc->thrown_feedback = (node_id << 16) | (i * 4 + 2);
        sc->closed_feedback = (node_id << 16) | (i * 4 + 3);
        sc->thrown_position = 16384;
        sc->closed_position = 49152;
        sc->throw_time = 30;  /* 3.0 seconds */
        sc->hold_time = 2;    /* 2 seconds, then cut PWM */
        sc->enabled = 1;
        sc->default_state = 0; /* thrown */
        sc->reversed = 0;
        /* _reserved and _node_pad already zeroed by memset above */
    }
}

void lcc_init(lcc_node_t *node, uint64_t node_id)
{
    memset(node, 0, sizeof(*node));
    node->node_id = node_id;
    node->state = LCC_STATE_ALIAS_CID;

    /* Try to load config from flash */
    bool loaded = nv_storage_init(&node->config, sizeof(node->config));

    if (loaded && node->config.magic == LCC_CONFIG_MAGIC) {
        DBG("LCC: loaded config from flash\n");
    } else {
        DBG("LCC: using default config\n");
        lcc_config_defaults(&node->config, node_id);
    }

    /* Set initial servo state from default_state config */
    for (int i = 0; i < LCC_NUM_SERVOS; i++)
        node->servo_state[i] = node->config.servos[i].default_state ? true : false;

    node->can_rx_queue = xQueueCreate(CAN_QUEUE_LEN, sizeof(lcc_frame_t));
    node->can_tx_queue = xQueueCreate(CAN_QUEUE_LEN, sizeof(lcc_frame_t));
    node->gc_tx_queue  = xQueueCreate(GC_QUEUE_LEN, sizeof(lcc_frame_t));
}

void lcc_send_datagram(lcc_node_t *node, uint16_t dst_alias,
                       const uint8_t *payload, uint8_t len)
{
    if (len <= 8) {
        /* Single-frame datagram */
        lcc_frame_t f;
        f.id = lcc_datagram_id(LCC_FRAME_DATAGRAM_ONE, dst_alias, node->alias);
        f.dlc = len;
        memcpy(f.data, payload, len);
        lcc_send(node, &f);
        return;
    }

    /* Multi-frame datagram */
    uint8_t offset = 0;

    /* First frame */
    lcc_frame_t f;
    f.id = lcc_datagram_id(LCC_FRAME_DATAGRAM_FIRST, dst_alias, node->alias);
    f.dlc = 8;
    memcpy(f.data, payload, 8);
    lcc_send(node, &f);
    offset = 8;

    while (offset < len) {
        uint8_t remaining = len - offset;
        bool last = (remaining <= 8);
        uint8_t chunk = last ? remaining : 8;

        f.id = lcc_datagram_id(last ? LCC_FRAME_DATAGRAM_FINAL
                                    : LCC_FRAME_DATAGRAM_MIDDLE,
                               dst_alias, node->alias);
        f.dlc = chunk;
        memcpy(f.data, payload + offset, chunk);
        lcc_send(node, &f);
        offset += chunk;
    }
}

void lcc_save_config(lcc_node_t *node)
{
    node->config.magic = LCC_CONFIG_MAGIC;

    if (nv_storage_write(&node->config, sizeof(node->config))) {
        DBG("LCC: config saved to flash\n");
    } else {
        DBG("LCC: config save failed\n");
    }
}

void lcc_task(void *param)
{
    lcc_node_t *node = (lcc_node_t *)param;

    if (!lcc_alias_allocate(node)) {
        DBG("LCC: alias allocation failed, task stopping\n");
        vTaskDelete(NULL);
        return;
    }

    /* Broadcast current state of all enabled servos so other nodes
     * (and JMRI) know the switch positions at startup. */
    for (int i = 0; i < LCC_NUM_SERVOS; i++) {
        if (!node->config.servos[i].enabled)
            continue;
        send_producer_identified(node, i, false);  /* thrown feedback */
        send_producer_identified(node, i, true);   /* closed feedback */
    }

    lcc_frame_t frame;
    while (1) {
        if (xQueueReceive(node->can_rx_queue, &frame, pdMS_TO_TICKS(20))) {
            lcc_handle_frame(node, &frame);
        }

        if (identify_led_poll()) {
            node->config.identify.enabled = 0;
            lcc_save_config(node);
        }

        /* Servo interpolation tick (~50Hz, runs every queue timeout) */
        uint8_t completed = servo_tick();
        for (int i = 0; i < LCC_NUM_SERVOS; i++) {
            if (!(completed & (1 << i)))
                continue;
            lcc_servo_config_t *sc = &node->config.servos[i];
            uint64_t fb = node->servo_state[i] ? sc->closed_feedback
                                                : sc->thrown_feedback;
            if (fb) {
                uint8_t buf[8];
                lcc_event_to_buf(fb, buf);
                lcc_send_global(node, LCC_MTI_EVENT_REPORT, buf, 8);
            }
            DBG("LCC: servo %d complete, feedback sent\n", i);
        }
    }
}
