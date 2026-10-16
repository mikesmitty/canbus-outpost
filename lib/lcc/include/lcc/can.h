#ifndef LCC_CAN_H
#define LCC_CAN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * OpenLCB over CAN: frame format and message type constants
 * (CAN Frame Transfer Standard, Message Network Standard).
 */

/* A 29-bit extended CAN frame. */
typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} lcc_frame_t;

/* CAN ID fields */
#define LCC_SRC_MASK              0x00000FFFu
#define LCC_VAR_FIELD_MASK        0x00FFF000u  /* MTI, or destination for datagrams */
#define LCC_VAR_FIELD_SHIFT       12
#define LCC_CAN_FRAME_TYPE_MASK   0x07000000u
#define LCC_CAN_FRAME_TYPE_SHIFT  24
#define LCC_OPENLCB_FRAME_BIT     0x08000000u  /* 1 = OpenLCB message, 0 = CAN control */
#define LCC_PRIORITY_BIT          0x10000000u  /* always 1 */
#define LCC_CONTROL_FIELD_MASK    0x07FFF000u
#define LCC_CONTROL_FIELD_SHIFT   12

/* CAN frame types (OpenLCB message frames) */
#define LCC_FRAME_GLOBAL_ADDRESSED  1
#define LCC_FRAME_DATAGRAM_ONE      2
#define LCC_FRAME_DATAGRAM_FIRST    3
#define LCC_FRAME_DATAGRAM_MIDDLE   4
#define LCC_FRAME_DATAGRAM_FINAL    5
#define LCC_FRAME_STREAM_DATA       7

/* CAN control frame fields */
#define LCC_CONTROL_RID  0x0700  /* Reserve ID */
#define LCC_CONTROL_AMD  0x0701  /* Alias Map Definition */
#define LCC_CONTROL_AME  0x0702  /* Alias Map Enquiry */
#define LCC_CONTROL_AMR  0x0703  /* Alias Map Reset */

/* Message Type Indicators */
#define LCC_MTI_INITIALIZATION_COMPLETE      0x0100
#define LCC_MTI_INITIALIZATION_COMPLETE_SIMPLE 0x0101
#define LCC_MTI_VERIFY_NODE_ID_ADDRESSED     0x0488
#define LCC_MTI_VERIFY_NODE_ID_GLOBAL        0x0490
#define LCC_MTI_VERIFIED_NODE_ID             0x0170
#define LCC_MTI_VERIFIED_NODE_ID_SIMPLE      0x0171
#define LCC_MTI_OPTIONAL_INTERACTION_REJECTED 0x0068
#define LCC_MTI_TERMINATE_DUE_TO_ERROR       0x00A8
#define LCC_MTI_PROTOCOL_SUPPORT_INQUIRY     0x0828
#define LCC_MTI_PROTOCOL_SUPPORT_REPLY       0x0668
#define LCC_MTI_CONSUMER_IDENTIFY            0x08F4
#define LCC_MTI_CONSUMER_IDENTIFIED_VALID    0x04C4
#define LCC_MTI_CONSUMER_IDENTIFIED_INVALID  0x04C5
#define LCC_MTI_CONSUMER_IDENTIFIED_UNKNOWN  0x04C7
#define LCC_MTI_PRODUCER_IDENTIFY            0x0914
#define LCC_MTI_PRODUCER_IDENTIFIED_VALID    0x0544
#define LCC_MTI_PRODUCER_IDENTIFIED_INVALID  0x0545
#define LCC_MTI_PRODUCER_IDENTIFIED_UNKNOWN  0x0547
#define LCC_MTI_PRODUCER_RANGE_IDENTIFIED    0x0524
#define LCC_MTI_EVENTS_IDENTIFY_ADDRESSED    0x0968
#define LCC_MTI_EVENTS_IDENTIFY_GLOBAL       0x0970
#define LCC_MTI_EVENT_REPORT                 0x05B4
#define LCC_MTI_SNIP_REQUEST                 0x0DE8
#define LCC_MTI_SNIP_REPLY                   0x0A08
#define LCC_MTI_DATAGRAM_OK                  0x0A28
#define LCC_MTI_DATAGRAM_REJECTED            0x0A48

#define LCC_MTI_ADDRESSED_BIT   0x0008

/* First payload byte of a multi-frame addressed message */
#define LCC_ADDR_NOT_FIRST_FRAME  0x20
#define LCC_ADDR_NOT_LAST_FRAME   0x10

/* Protocol Support Inquiry bits (48-bit field, sent big-endian) */
#define LCC_PROTOCOL_SIMPLE_SUBSET     (1ULL << 47)
#define LCC_PROTOCOL_DATAGRAM          (1ULL << 46)
#define LCC_PROTOCOL_STREAM            (1ULL << 45)
#define LCC_PROTOCOL_MEMORY_CONFIG     (1ULL << 44)
#define LCC_PROTOCOL_RESERVATION       (1ULL << 43)
#define LCC_PROTOCOL_EVENT_EXCHANGE    (1ULL << 42)
#define LCC_PROTOCOL_IDENTIFICATION    (1ULL << 41)
#define LCC_PROTOCOL_TEACH_LEARN       (1ULL << 40)
#define LCC_PROTOCOL_REMOTE_BUTTON     (1ULL << 39)
#define LCC_PROTOCOL_ABBREVIATED_CDI   (1ULL << 38)
#define LCC_PROTOCOL_DISPLAY           (1ULL << 37)
#define LCC_PROTOCOL_SNIP              (1ULL << 36)
#define LCC_PROTOCOL_CDI               (1ULL << 35)
#define LCC_PROTOCOL_FIRMWARE_UPGRADE         (1ULL << 29)
#define LCC_PROTOCOL_FIRMWARE_UPGRADE_ACTIVE  (1ULL << 28)

/* Error codes (Datagram Rejected, Optional Interaction Rejected, replies) */
#define LCC_ERROR_PERMANENT               0x1000
#define LCC_ERROR_UNIMPLEMENTED           0x1040
#define LCC_ERROR_UNIMPLEMENTED_SUBCMD    0x1041
#define LCC_ERROR_UNIMPLEMENTED_CMD       0x1042  /* e.g. unknown datagram type */
#define LCC_ERROR_UNIMPLEMENTED_MTI       0x1043
#define LCC_ERROR_INVALID_ARGS            0x1080
#define LCC_ERROR_TEMPORARY               0x2000
#define LCC_ERROR_BUFFER_UNAVAILABLE      0x2020
#define LCC_ERROR_OUT_OF_ORDER            0x2040

static inline uint32_t lcc_message_id(uint16_t mti, uint16_t src_alias)
{
    return LCC_PRIORITY_BIT | LCC_OPENLCB_FRAME_BIT
         | ((uint32_t)LCC_FRAME_GLOBAL_ADDRESSED << LCC_CAN_FRAME_TYPE_SHIFT)
         | ((uint32_t)(mti & 0xFFF) << LCC_VAR_FIELD_SHIFT)
         | (src_alias & LCC_SRC_MASK);
}

static inline uint32_t lcc_datagram_id(uint8_t frame_type, uint16_t dst_alias,
                                       uint16_t src_alias)
{
    return LCC_PRIORITY_BIT | LCC_OPENLCB_FRAME_BIT
         | ((uint32_t)(frame_type & 0x7) << LCC_CAN_FRAME_TYPE_SHIFT)
         | ((uint32_t)(dst_alias & 0xFFF) << LCC_VAR_FIELD_SHIFT)
         | (src_alias & LCC_SRC_MASK);
}

/* CID frames carry a sequence number (7..4) in the frame type position. */
static inline uint32_t lcc_control_id(uint16_t field, uint8_t sequence,
                                      uint16_t src_alias)
{
    return LCC_PRIORITY_BIT
         | ((uint32_t)(sequence & 0x7) << LCC_CAN_FRAME_TYPE_SHIFT)
         | ((uint32_t)(field & 0xFFF) << LCC_CONTROL_FIELD_SHIFT)
         | (src_alias & LCC_SRC_MASK);
}

static inline uint16_t lcc_frame_src(uint32_t id)
{
    return id & LCC_SRC_MASK;
}

static inline bool lcc_is_control_frame(uint32_t id)
{
    return (id & LCC_OPENLCB_FRAME_BIT) == 0;
}

static inline bool lcc_is_cid_frame(uint32_t id)
{
    return lcc_is_control_frame(id) &&
           ((id >> LCC_CAN_FRAME_TYPE_SHIFT) & 0x4) != 0;
}

static inline uint16_t lcc_control_field(uint32_t id)
{
    return (id & LCC_CONTROL_FIELD_MASK) >> LCC_CONTROL_FIELD_SHIFT;
}

static inline uint8_t lcc_can_frame_type(uint32_t id)
{
    return (id & LCC_CAN_FRAME_TYPE_MASK) >> LCC_CAN_FRAME_TYPE_SHIFT;
}

static inline uint16_t lcc_frame_mti(uint32_t id)
{
    return (id & LCC_VAR_FIELD_MASK) >> LCC_VAR_FIELD_SHIFT;
}

static inline uint16_t lcc_datagram_dst(uint32_t id)
{
    return (id & LCC_VAR_FIELD_MASK) >> LCC_VAR_FIELD_SHIFT;
}

#endif /* LCC_CAN_H */
