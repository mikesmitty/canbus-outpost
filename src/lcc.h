#ifndef LCC_H
#define LCC_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "lcc_defs.h"

/* Number of servo channels / event pairs */
#define LCC_NUM_SERVOS  4
#define LCC_NUM_EVENTS  (LCC_NUM_SERVOS * 2)

/* Config magic for flash validation — bump when layout changes */
#define LCC_CONFIG_MAGIC  0x4C434303  /* "LCC\x03" */

#define LCC_IDENTIFY_TIMEOUT_INDEFINITE  0
#define LCC_IDENTIFY_TIMEOUT_1_MIN       1
#define LCC_IDENTIFY_TIMEOUT_5_MIN       5
#define LCC_IDENTIFY_TIMEOUT_10_MIN      10
#define LCC_IDENTIFY_TIMEOUT_15_MIN      15

/* Per-servo configuration (48 bytes, naturally aligned to 8 bytes).
 * Layout must match CDI XML field sizes exactly (group stride = 48). */
typedef struct {
    uint64_t throw_event;       /* Command event: throw */
    uint64_t close_event;       /* Command event: close */
    uint64_t thrown_feedback;    /* Feedback event: thrown complete */
    uint64_t closed_feedback;   /* Feedback event: closed complete */
    uint16_t thrown_position;   /* PWM position for thrown (0-65535) */
    uint16_t closed_position;   /* PWM position for closed (0-65535) */
    uint8_t  throw_time;        /* Throw duration in 0.1s (0=instant, 1-255 = 0.1-25.5s) */
    uint8_t  hold_time;         /* Hold time after throw in seconds (0=hold forever) */
    uint8_t  enabled;           /* 0=disabled, 1=enabled */
    uint8_t  default_state;     /* 0=thrown, 1=closed */
    uint8_t  reversed;          /* 0=normal, 1=reversed */
    uint8_t  _reserved[7];      /* pad to 48 bytes (multiple of 8) */
} lcc_servo_config_t;

typedef struct {
    uint8_t enabled;            /* 0=disabled, 1=enabled */
    uint8_t timeout_minutes;    /* 0=indefinite, otherwise minutes */
    uint8_t _reserved[2];       /* pad to 4 bytes */
} lcc_identify_config_t;

/* Persistent config stored in flash (328 bytes, naturally aligned). */
typedef struct {
    uint32_t magic;
    uint8_t  user_name[64];
    uint8_t  user_desc[64];
    lcc_identify_config_t identify;
    lcc_servo_config_t servos[LCC_NUM_SERVOS];
} lcc_config_t;

_Static_assert(sizeof(lcc_servo_config_t) == 48, "servo config must be 48 bytes for CDI");
_Static_assert(sizeof(lcc_config_t) == 328, "config struct size mismatch");

/* Node states */
enum {
    LCC_STATE_ALIAS_CID,
    LCC_STATE_ALIAS_WAIT,
    LCC_STATE_INITIALIZED,
};

/* SNIP strings */
#define LCC_SNIP_MFG_NAME      "mikesmitty"
#define LCC_SNIP_MODEL_NAME    "CAN Bus Outpost"
#define LCC_SNIP_HW_VERSION    "Rev A"
#ifndef LCC_SW_VERSION
// x-release-please-start-version
#define LCC_SNIP_SW_VERSION    "0.1.0"
// x-release-please-end
#else
#define LCC_SNIP_SW_VERSION    LCC_SW_VERSION
#endif

/* Datagram reassembly buffer */
#define LCC_DATAGRAM_MAX  72

typedef struct {
    uint16_t src_alias;
    uint8_t  buf[LCC_DATAGRAM_MAX];
    uint8_t  len;
    bool     active;
} lcc_datagram_rx_t;

typedef struct {
    uint64_t node_id;
    uint16_t alias;
    uint8_t  state;

    /* Full configuration (loaded from flash, written by CDI) */
    lcc_config_t config;

    /* Runtime servo state: true = closed */
    bool servo_state[LCC_NUM_SERVOS];

    /* Datagram reassembly */
    lcc_datagram_rx_t dg_rx;

    /* FreeRTOS queues */
    QueueHandle_t can_rx_queue;
    QueueHandle_t can_tx_queue;
    QueueHandle_t gc_tx_queue;
} lcc_node_t;

/* Fill config with factory defaults. node_id is used to derive default event IDs. */
void lcc_config_defaults(lcc_config_t *config, uint64_t node_id);

/* Initialize the LCC node structure.
 * Loads config from flash if valid, otherwise uses defaults.
 * Queues are created internally. Call before starting tasks. */
void lcc_init(lcc_node_t *node, uint64_t node_id);

/* Save current config to flash. */
void lcc_save_config(lcc_node_t *node);

/* Send a datagram (fragments into CAN frames automatically). */
void lcc_send_datagram(lcc_node_t *node, uint16_t dst_alias,
                       const uint8_t *payload, uint8_t len);

/* LCC protocol task — runs alias allocation then main loop.
 * Pass lcc_node_t* as the task parameter. */
void lcc_task(void *param);

#endif /* LCC_H */
