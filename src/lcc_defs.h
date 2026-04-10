#ifndef LCC_DEFS_H
#define LCC_DEFS_H

#include <stdint.h>
#include <stdbool.h>

/*
 * LCC/OpenLCB CAN protocol constants.
 * Reference: OpenMRN CanDefs.hxx, Defs.hxx
 */

/* CAN ID field masks and shifts (29-bit extended frame) */
#define LCC_SRC_MASK              0x00000FFF
#define LCC_SRC_SHIFT             0
#define LCC_MTI_MASK              0x00FFF000
#define LCC_MTI_SHIFT             12
#define LCC_DST_MASK              0x00FFF000
#define LCC_DST_SHIFT             12
#define LCC_CAN_FRAME_TYPE_MASK   0x07000000
#define LCC_CAN_FRAME_TYPE_SHIFT  24
#define LCC_FRAME_TYPE_MASK       0x08000000
#define LCC_FRAME_TYPE_SHIFT      27
#define LCC_PRIORITY_MASK         0x10000000
#define LCC_PRIORITY_SHIFT        28

/* Control frame field masks */
#define LCC_CONTROL_FIELD_MASK    0x07FFF000
#define LCC_CONTROL_FIELD_SHIFT   12

/* CAN frame types */
#define LCC_FRAME_GLOBAL_ADDRESSED      1
#define LCC_FRAME_DATAGRAM_ONE          2
#define LCC_FRAME_DATAGRAM_FIRST        3
#define LCC_FRAME_DATAGRAM_MIDDLE       4
#define LCC_FRAME_DATAGRAM_FINAL        5
#define LCC_FRAME_STREAM_DATA           7

/* Frame type */
#define LCC_FRAME_TYPE_CONTROL  0
#define LCC_FRAME_TYPE_OPENLCB  1

/* Priority */
#define LCC_PRIORITY_HIGH    0
#define LCC_PRIORITY_NORMAL  1

/* Control frame field values */
#define LCC_CONTROL_RID  0x0700
#define LCC_CONTROL_AMD  0x0701
#define LCC_CONTROL_AME  0x0702
#define LCC_CONTROL_AMR  0x0703

/* Message Type Indicators (MTIs) */
#define LCC_MTI_INITIALIZATION_COMPLETE    0x0100
#define LCC_MTI_VERIFY_NODE_ID_GLOBAL      0x0490
#define LCC_MTI_VERIFY_NODE_ID_ADDRESSED   0x0488
#define LCC_MTI_VERIFIED_NODE_ID           0x0170
#define LCC_MTI_PROTOCOL_SUPPORT_INQUIRY   0x0828
#define LCC_MTI_PROTOCOL_SUPPORT_REPLY     0x0668
#define LCC_MTI_CONSUMER_IDENTIFY          0x08F4
#define LCC_MTI_CONSUMER_IDENTIFIED_VALID  0x04C4
#define LCC_MTI_CONSUMER_IDENTIFIED_INVALID 0x04C5
#define LCC_MTI_CONSUMER_IDENTIFIED_UNKNOWN 0x04C7
#define LCC_MTI_PRODUCER_IDENTIFY          0x0914
#define LCC_MTI_PRODUCER_IDENTIFIED_VALID  0x0544
#define LCC_MTI_PRODUCER_IDENTIFIED_INVALID 0x0545
#define LCC_MTI_PRODUCER_IDENTIFIED_UNKNOWN 0x0547
#define LCC_MTI_EVENTS_IDENTIFY_ADDRESSED  0x0968
#define LCC_MTI_EVENTS_IDENTIFY_GLOBAL     0x0970
#define LCC_MTI_EVENT_REPORT               0x05B4
#define LCC_MTI_IDENT_INFO_REQUEST         0x0DE8
#define LCC_MTI_IDENT_INFO_REPLY           0x0A08
#define LCC_MTI_OPT_INTERACTION_REJECTED   0x0068
#define LCC_MTI_DATAGRAM_OK                0x0A28
#define LCC_MTI_DATAGRAM_REJECTED          0x0A48

/* MTI flag bits */
#define LCC_MTI_ADDRESS_MASK  0x0008
#define LCC_MTI_EVENT_MASK    0x0004

/* Addressed message continuation flags (upper nibble of first payload byte) */
#define LCC_NOT_FIRST_FRAME  0x20
#define LCC_NOT_LAST_FRAME   0x10

/* Protocol Identification Protocol (PIP) bits */
#define LCC_PIP_SIMPLE_PROTOCOL     (1ULL << 47)
#define LCC_PIP_DATAGRAM            (1ULL << 46)
#define LCC_PIP_STREAM              (1ULL << 45)
#define LCC_PIP_MEMORY_CONFIG       (1ULL << 44)
#define LCC_PIP_RESERVATION         (1ULL << 43)
#define LCC_PIP_EVENT_EXCHANGE      (1ULL << 42)
#define LCC_PIP_IDENTIFICATION      (1ULL << 41)
#define LCC_PIP_LEARN_CONFIG        (1ULL << 40)
#define LCC_PIP_REMOTE_BUTTON       (1ULL << 39)
#define LCC_PIP_ABBREVIATED_CDI     (1ULL << 38)
#define LCC_PIP_DISPLAY             (1ULL << 37)
#define LCC_PIP_SIMPLE_NODE_INFO    (1ULL << 36)
#define LCC_PIP_CDI                 (1ULL << 35)

/* Memory Configuration protocol command byte */
#define LCC_MEMCONFIG_CMD           0x20

/* Memory Config sub-commands (byte 1) */
#define LCC_MEMCFG_WRITE            0x00  /* Write, space in low 2 bits */
#define LCC_MEMCFG_READ             0x40  /* Read, space in low 2 bits */
#define LCC_MEMCFG_READ_REPLY       0x50  /* Read reply, space in low 2 bits */
#define LCC_MEMCFG_WRITE_REPLY      0x10  /* Write reply */
#define LCC_MEMCFG_GET_SPACE_INFO   0x84  /* Get Address Space Info */
#define LCC_MEMCFG_SPACE_INFO_REPLY 0x87  /* Address Space Info reply (present) */
#define LCC_MEMCFG_SPACE_NOT_KNOWN  0x86  /* Address Space Info reply (not present) */
#define LCC_MEMCFG_UPDATE_COMPLETE  0xA8  /* Indicate Configuration Update Complete */
#define LCC_MEMCFG_REBOOT          0xA9  /* Reboot node */
#define LCC_MEMCFG_FACTORY_RESET   0xAA  /* Factory Reset (payload = 6-byte node ID) */

/* Read/Write sub-command: space encoding in low 2 bits */
#define LCC_MEMCFG_SPACE_SPECIAL    0x03  /* space in low 2 bits means 0xFF */
/* 0x03 = 0xFF (CDI), 0x02 = 0xFE, 0x01 = 0xFD (config), 0x00 = explicit byte */

/* LCC CAN bitrate */
#define LCC_CAN_BITRATE  125000

/* Internal CAN frame representation */
typedef struct {
    uint32_t id;        /* 29-bit CAN ID (always extended, no EFF flag) */
    uint8_t  dlc;
    uint8_t  data[8];
} lcc_frame_t;

/* Helper: build a normal OpenLCB CAN ID */
static inline uint32_t lcc_can_id(uint16_t mti, uint16_t src_alias)
{
    return (src_alias & 0xFFF)
         | ((uint32_t)mti << LCC_MTI_SHIFT)
         | ((uint32_t)LCC_FRAME_GLOBAL_ADDRESSED << LCC_CAN_FRAME_TYPE_SHIFT)
         | ((uint32_t)LCC_FRAME_TYPE_OPENLCB << LCC_FRAME_TYPE_SHIFT)
         | ((uint32_t)LCC_PRIORITY_NORMAL << LCC_PRIORITY_SHIFT);
}

/* Helper: build a control frame CAN ID */
static inline uint32_t lcc_control_id(uint16_t src_alias, uint16_t field,
                                      uint8_t sequence)
{
    return (src_alias & 0xFFF)
         | ((uint32_t)(field & 0xFFF) << LCC_CONTROL_FIELD_SHIFT)
         | ((uint32_t)(sequence & 0x7) << LCC_CAN_FRAME_TYPE_SHIFT)
         | ((uint32_t)LCC_FRAME_TYPE_CONTROL << LCC_FRAME_TYPE_SHIFT)
         | ((uint32_t)LCC_PRIORITY_NORMAL << LCC_PRIORITY_SHIFT);
}

/* Helper: extract source alias from CAN ID */
static inline uint16_t lcc_get_src(uint32_t can_id)
{
    return can_id & LCC_SRC_MASK;
}

/* Helper: extract MTI from CAN ID */
static inline uint16_t lcc_get_mti(uint32_t can_id)
{
    return (can_id & LCC_MTI_MASK) >> LCC_MTI_SHIFT;
}

/* Helper: extract CAN frame type */
static inline uint8_t lcc_get_can_frame_type(uint32_t can_id)
{
    return (can_id & LCC_CAN_FRAME_TYPE_MASK) >> LCC_CAN_FRAME_TYPE_SHIFT;
}

/* Helper: is this a control frame? */
static inline bool lcc_is_control_frame(uint32_t can_id)
{
    return ((can_id & LCC_FRAME_TYPE_MASK) >> LCC_FRAME_TYPE_SHIFT)
            == LCC_FRAME_TYPE_CONTROL;
}

/* Helper: is this a CID frame? */
static inline bool lcc_is_cid_frame(uint32_t can_id)
{
    return ((can_id >> LCC_CAN_FRAME_TYPE_SHIFT) & 0x1C) == 0x14;
}

/* Helper: extract control field */
static inline uint16_t lcc_get_control_field(uint32_t can_id)
{
    return (can_id & LCC_CONTROL_FIELD_MASK) >> LCC_CONTROL_FIELD_SHIFT;
}

/* Helper: build a datagram CAN ID */
static inline uint32_t lcc_datagram_id(uint8_t frame_type, uint16_t dst_alias,
                                       uint16_t src_alias)
{
    return (src_alias & 0xFFF)
         | ((uint32_t)(dst_alias & 0xFFF) << 12)
         | ((uint32_t)frame_type << LCC_CAN_FRAME_TYPE_SHIFT)
         | ((uint32_t)LCC_FRAME_TYPE_OPENLCB << LCC_FRAME_TYPE_SHIFT)
         | ((uint32_t)LCC_PRIORITY_NORMAL << LCC_PRIORITY_SHIFT);
}

/* Helper: extract destination alias from datagram CAN ID */
static inline uint16_t lcc_get_datagram_dst(uint32_t can_id)
{
    return (can_id >> 12) & 0xFFF;
}

/* Helper: write 48-bit node ID into buffer (big-endian) */
static inline void lcc_node_id_to_buf(uint64_t node_id, uint8_t *buf)
{
    buf[0] = (node_id >> 40) & 0xFF;
    buf[1] = (node_id >> 32) & 0xFF;
    buf[2] = (node_id >> 24) & 0xFF;
    buf[3] = (node_id >> 16) & 0xFF;
    buf[4] = (node_id >>  8) & 0xFF;
    buf[5] = (node_id >>  0) & 0xFF;
}

/* Helper: read 64-bit event ID from buffer (big-endian) */
static inline uint64_t lcc_event_from_buf(const uint8_t *buf)
{
    return ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48)
         | ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32)
         | ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16)
         | ((uint64_t)buf[6] <<  8) | ((uint64_t)buf[7]);
}

/* Helper: write 64-bit event ID to buffer (big-endian) */
static inline void lcc_event_to_buf(uint64_t event_id, uint8_t *buf)
{
    buf[0] = (event_id >> 56) & 0xFF;
    buf[1] = (event_id >> 48) & 0xFF;
    buf[2] = (event_id >> 40) & 0xFF;
    buf[3] = (event_id >> 32) & 0xFF;
    buf[4] = (event_id >> 24) & 0xFF;
    buf[5] = (event_id >> 16) & 0xFF;
    buf[6] = (event_id >>  8) & 0xFF;
    buf[7] = (event_id >>  0) & 0xFF;
}

#endif /* LCC_DEFS_H */
