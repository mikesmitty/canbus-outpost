#include <string.h>
#include "lcc.h"
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
}

static void handle_pip(lcc_node_t *node, const lcc_frame_t *f)
{
    if (f->dlc < 2)
        return;
    uint16_t dst = ((f->data[0] & 0x0F) << 8) | f->data[1];
    if (dst != node->alias)
        return;

    uint64_t protocols = LCC_PIP_SIMPLE_PROTOCOL
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
    snip[pos++] = 0; /* user name (empty) */
    snip[pos++] = 0; /* user description (empty) */

    uint16_t src = lcc_get_src(f->id);
    lcc_send_addressed(node, LCC_MTI_IDENT_INFO_REPLY, src, snip, pos);
}

static void send_consumer_identified(lcc_node_t *node, int event_idx)
{
    uint8_t buf[8];
    lcc_event_to_buf(node->events[event_idx], buf);

    int servo = event_idx / 2;
    bool is_close = (event_idx & 1);
    bool active = (is_close == node->servo_state[servo]);

    uint16_t mti = active ? LCC_MTI_CONSUMER_IDENTIFIED_VALID
                          : LCC_MTI_CONSUMER_IDENTIFIED_INVALID;
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

    for (int i = 0; i < LCC_NUM_EVENTS; i++)
        send_consumer_identified(node, i);
}

static void handle_consumer_identify(lcc_node_t *node, const lcc_frame_t *f)
{
    if (f->dlc < 8)
        return;

    uint64_t event_id = lcc_event_from_buf(f->data);

    for (int i = 0; i < LCC_NUM_EVENTS; i++) {
        if (node->events[i] == event_id) {
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
        if (event_id == node->events[i * 2]) {
            /* Throw event */
            servo_set_position(i, node->servo_thrown[i]);
            node->servo_state[i] = false;
            DBG("LCC: servo %d thrown\n", i);
            return;
        } else if (event_id == node->events[i * 2 + 1]) {
            /* Close event */
            servo_set_position(i, node->servo_closed[i]);
            node->servo_state[i] = true;
            DBG("LCC: servo %d closed\n", i);
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

static void handle_datagram(lcc_node_t *node, const lcc_frame_t *f)
{
    /* We don't support any datagram protocols yet — reject */
    uint16_t src = lcc_get_src(f->id);

    uint8_t reject[4];
    reject[0] = ((src >> 8) & 0x0F);
    reject[1] = src & 0xFF;
    reject[2] = 0x10;  /* permanent error */
    reject[3] = 0x43;  /* not implemented */

    lcc_frame_t resp;
    resp.id = lcc_can_id(LCC_MTI_DATAGRAM_REJECTED, node->alias);
    resp.dlc = 4;
    memcpy(resp.data, reject, 4);
    lcc_send(node, &resp);
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

void lcc_init(lcc_node_t *node, uint64_t node_id)
{
    memset(node, 0, sizeof(*node));
    node->node_id = node_id;
    node->state = LCC_STATE_ALIAS_CID;

    /* Try to load config from flash */
    lcc_config_t config;
    bool loaded = nv_storage_init(&config, sizeof(config));

    if (loaded && config.magic == LCC_CONFIG_MAGIC) {
        DBG("LCC: loaded config from flash\n");
        memcpy(node->events, config.events, sizeof(node->events));
        memcpy(node->servo_thrown, config.servo_thrown, sizeof(node->servo_thrown));
        memcpy(node->servo_closed, config.servo_closed, sizeof(node->servo_closed));
    } else {
        DBG("LCC: using default config\n");
        /* Default event IDs: node_id base with servo index */
        for (int i = 0; i < LCC_NUM_SERVOS; i++) {
            node->events[i * 2]     = (node_id << 16) | (i * 2);
            node->events[i * 2 + 1] = (node_id << 16) | (i * 2 + 1);
        }
        /* Default servo endpoints */
        for (int i = 0; i < LCC_NUM_SERVOS; i++) {
            node->servo_thrown[i] = 16384;
            node->servo_closed[i] = 49152;
        }
    }

    for (int i = 0; i < LCC_NUM_SERVOS; i++)
        node->servo_state[i] = false;

    node->can_rx_queue = xQueueCreate(CAN_QUEUE_LEN, sizeof(lcc_frame_t));
    node->can_tx_queue = xQueueCreate(CAN_QUEUE_LEN, sizeof(lcc_frame_t));
    node->gc_tx_queue  = xQueueCreate(GC_QUEUE_LEN, sizeof(lcc_frame_t));
}

void lcc_save_config(lcc_node_t *node)
{
    lcc_config_t config;
    config.magic = LCC_CONFIG_MAGIC;
    memcpy(config.events, node->events, sizeof(config.events));
    memcpy(config.servo_thrown, node->servo_thrown, sizeof(config.servo_thrown));
    memcpy(config.servo_closed, node->servo_closed, sizeof(config.servo_closed));

    if (nv_storage_write(&config, sizeof(config))) {
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

    lcc_frame_t frame;
    while (1) {
        if (xQueueReceive(node->can_rx_queue, &frame, pdMS_TO_TICKS(100))) {
            lcc_handle_frame(node, &frame);
        }
    }
}
