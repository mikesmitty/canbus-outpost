#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/irq.h"
#include "FreeRTOS.h"
#include "task.h"
#include "board.h"
#include "can2040.h"
#include "servo.h"
#include "lcc.h"
#include "gridconnect.h"
#include "util/dbg.h"

/* ------------------------------------------------------------------ */
/* CAN bus (can2040 on PIO1)                                          */
/* ------------------------------------------------------------------ */

static struct can2040 cbus;
static lcc_node_t lcc_node;

static void can2040_cb(struct can2040 *cd, uint32_t notify,
                       struct can2040_msg *msg)
{
    (void)cd;
    if (notify == CAN2040_NOTIFY_RX) {
        lcc_frame_t frame;
        frame.id = msg->id & 0x1FFFFFFF;  /* strip EFF flag */
        frame.dlc = (uint8_t)msg->dlc;
        memcpy(frame.data, msg->data, 8);

        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(lcc_node.can_rx_queue, &frame, &woken);
        xQueueSendFromISR(lcc_node.gc_tx_queue, &frame, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void PIO1_IRQHandler(void)
{
    can2040_pio_irq_handler(&cbus);
}

static void can_init(void)
{
    can2040_setup(&cbus, 1); /* PIO1 */
    can2040_callback_config(&cbus, can2040_cb);

    uint32_t pio1_irq = PIO1_IRQ_0;
    irq_set_exclusive_handler(pio1_irq, PIO1_IRQHandler);
    irq_set_priority(pio1_irq, configMAX_SYSCALL_INTERRUPT_PRIORITY);
    irq_set_enabled(pio1_irq, true);

    can2040_start(&cbus, configCPU_CLOCK_HZ, LCC_CAN_BITRATE,
                  CAN_RX_PIN, CAN_TX_PIN);

    DBG("CAN: initialized on PIO1, %d bps\n", LCC_CAN_BITRATE);
}

/* ------------------------------------------------------------------ */
/* CAN TX task — drains can_tx_queue and transmits via can2040         */
/* ------------------------------------------------------------------ */

static void can_tx_task(void *param)
{
    lcc_node_t *node = (lcc_node_t *)param;
    lcc_frame_t frame;

    while (1) {
        if (xQueueReceive(node->can_tx_queue, &frame, portMAX_DELAY)) {
            struct can2040_msg msg;
            msg.id = frame.id | CAN2040_ID_EFF;
            msg.dlc = frame.dlc;
            memcpy(msg.data, frame.data, 8);

            /* Retry until transmit buffer available */
            while (can2040_transmit(&cbus, &msg) < 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* GridConnect task — USB CDC bridge                                   */
/* ------------------------------------------------------------------ */

static void gc_task(void *param)
{
    lcc_node_t *node = (lcc_node_t *)param;
    gc_parser_t parser;
    memset(&parser, 0, sizeof(parser));

    while (1) {
        /* TX: drain gc_tx_queue → encode → USB */
        lcc_frame_t frame;
        while (xQueueReceive(node->gc_tx_queue, &frame, 0) == pdTRUE) {
            char buf[GC_MAX_LEN];
            int len = gc_encode(&frame, buf, sizeof(buf));
            if (len > 0) {
                for (int i = 0; i < len; i++)
                    putchar_raw(buf[i]);
            }
        }

        /* RX: USB → parse GridConnect → inject to CAN + LCC */
        int c = getchar_timeout_us(0);
        while (c != PICO_ERROR_TIMEOUT) {
            lcc_frame_t rx_frame;
            if (gc_parse_byte(&parser, (char)c, &rx_frame)) {
                xQueueSend(node->can_rx_queue, &rx_frame, 0);
                xQueueSend(node->can_tx_queue, &rx_frame, 0);
            }
            c = getchar_timeout_us(0);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ------------------------------------------------------------------ */
/* Heartbeat task                                                     */
/* ------------------------------------------------------------------ */

static void heartbeat_task(void *param)
{
    (void)param;
    gpio_init(HEARTBEAT_LED);
    gpio_set_dir(HEARTBEAT_LED, GPIO_OUT);

    while (1) {
        gpio_put(HEARTBEAT_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_put(HEARTBEAT_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(9900));
    }
}

/* ------------------------------------------------------------------ */
/* Stack overflow hook                                                */
/* ------------------------------------------------------------------ */

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    DBG("Stack overflow in task: %s\n", pcTaskName);
    __breakpoint();
    while (1) {}
}

/* ------------------------------------------------------------------ */
/* Node ID from board unique ID                                       */
/* ------------------------------------------------------------------ */

static uint64_t get_node_id(void)
{
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);

    /* Use bottom 6 bytes as 48-bit LCC node ID */
    uint64_t nid = 0;
    for (int i = 2; i < 8; i++)
        nid = (nid << 8) | board_id.id[i];

    return nid;
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    stdio_init_all();

    DBG("canbus-outpost starting (built " __DATE__ " " __TIME__ ")\n");

    servo_init();

    uint64_t node_id = get_node_id();
    DBG("Node ID: %012llX\n", node_id);

    lcc_init(&lcc_node, node_id);
    can_init();

    xTaskCreate(lcc_task, "lcc", 2048, &lcc_node, 2, NULL);
    xTaskCreate(can_tx_task, "can_tx", 512, &lcc_node, 3, NULL);
    xTaskCreate(gc_task, "gc", 1024, &lcc_node, 1, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 256, NULL, 0, NULL);

    vTaskStartScheduler();

    return 0;
}
