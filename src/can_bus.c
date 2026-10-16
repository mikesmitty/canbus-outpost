#include "can_bus.h"
#include <string.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "board.h"
#include "can2040.h"
#include "hardware/irq.h"
#include "lcc_task.h"
#include "usb_gridconnect.h"
#include "util/dbg.h"

#define LCC_CAN_BITRATE  125000
#define TX_QUEUE_LEN     64
#define SEND_WAIT_MS     20  // backpressure while the bus drains a long reply

static struct can2040 cbus;
static QueueHandle_t tx_queue;
static TaskHandle_t tx_task;
static volatile uint32_t bus_errors;

static void can2040_cb(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg)
{
    (void)cd;
    BaseType_t woken = pdFALSE;

    if (notify == CAN2040_NOTIFY_RX) {
        // LCC uses only extended data frames
        if ((msg->id & CAN2040_ID_EFF) && !(msg->id & CAN2040_ID_RTR)) {
            lcc_frame_t frame = { .id = msg->id & 0x1FFFFFFF,
                                  .dlc = (uint8_t)(msg->dlc > 8 ? 8 : msg->dlc) };
            memcpy(frame.data, msg->data, frame.dlc);
            lcc_task_post_frame_from_isr(&frame, &woken);
            usb_gc_send_from_isr(&frame, &woken);
        }
    } else if (notify == CAN2040_NOTIFY_TX) {
        if (tx_task)
            vTaskNotifyGiveFromISR(tx_task, &woken);
    } else if (notify == CAN2040_NOTIFY_ERROR) {
        bus_errors++;
    }

    portYIELD_FROM_ISR(woken);
}

static void pio1_irq_handler(void)
{
    can2040_pio_irq_handler(&cbus);
}

void can_bus_init(void)
{
    tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(lcc_frame_t));
}

void can_bus_task(void *param)
{
    (void)param;
    tx_task = xTaskGetCurrentTaskHandle();

    can2040_setup(&cbus, 1);
    can2040_callback_config(&cbus, can2040_cb);
    irq_set_exclusive_handler(PIO1_IRQ_0, pio1_irq_handler);
    irq_set_priority(PIO1_IRQ_0, configMAX_SYSCALL_INTERRUPT_PRIORITY);
    irq_set_enabled(PIO1_IRQ_0, true);
    can2040_start(&cbus, configCPU_CLOCK_HZ, LCC_CAN_BITRATE, CAN_RX_PIN, CAN_TX_PIN);
    DBG("CAN: started on PIO1 at %d bps\n", LCC_CAN_BITRATE);

    for (;;) {
        lcc_frame_t frame;
        xQueueReceive(tx_queue, &frame, portMAX_DELAY);

        // A transmit-complete notification makes room
        while (!can2040_check_transmit(&cbus))
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        struct can2040_msg msg = { .id = frame.id | CAN2040_ID_EFF, .dlc = frame.dlc };
        memcpy(msg.data, frame.data, frame.dlc);
        can2040_transmit(&cbus, &msg);
    }
}

bool can_bus_send(const lcc_frame_t *frame)
{
    return xQueueSend(tx_queue, frame, pdMS_TO_TICKS(SEND_WAIT_MS)) == pdTRUE;
}
