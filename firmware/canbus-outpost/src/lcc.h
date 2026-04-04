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

/* Config magic for flash validation */
#define LCC_CONFIG_MAGIC  0x4C434301  /* "LCC\x01" */

/* Persistent config stored in flash */
typedef struct {
    uint32_t magic;
    uint64_t events[LCC_NUM_EVENTS];
    uint16_t servo_thrown[LCC_NUM_SERVOS];
    uint16_t servo_closed[LCC_NUM_SERVOS];
} lcc_config_t;

/* Node states */
enum {
    LCC_STATE_ALIAS_CID,
    LCC_STATE_ALIAS_WAIT,
    LCC_STATE_INITIALIZED,
};

/* SNIP strings */
#define LCC_SNIP_MFG_NAME      "Canbus Outpost"
#define LCC_SNIP_MODEL_NAME    "Servo Switch Controller"
#define LCC_SNIP_HW_VERSION    "Rev A"
#define LCC_SNIP_SW_VERSION    "1.0"

typedef struct {
    uint64_t node_id;
    uint16_t alias;
    uint8_t  state;

    /* Event consumers: events[i*2] = throw, events[i*2+1] = close */
    uint64_t events[LCC_NUM_EVENTS];
    bool     servo_state[LCC_NUM_SERVOS]; /* true = closed */

    /* Servo PWM endpoints */
    uint16_t servo_thrown[LCC_NUM_SERVOS];
    uint16_t servo_closed[LCC_NUM_SERVOS];

    /* FreeRTOS queues */
    QueueHandle_t can_rx_queue;
    QueueHandle_t can_tx_queue;
    QueueHandle_t gc_tx_queue;
} lcc_node_t;

/* Initialize the LCC node structure.
 * Loads config from flash if valid, otherwise uses defaults.
 * Queues are created internally. Call before starting tasks. */
void lcc_init(lcc_node_t *node, uint64_t node_id);

/* Save current event IDs and servo positions to flash. */
void lcc_save_config(lcc_node_t *node);

/* LCC protocol task — runs alias allocation then main loop.
 * Pass lcc_node_t* as the task parameter. */
void lcc_task(void *param);

#endif /* LCC_H */
