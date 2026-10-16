#include "usb_gridconnect.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "queue.h"
#include "task.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "app/app.h"
#include "can_bus.h"
#include "lcc/gridconnect.h"
#include "lcc_task.h"
#include "node_id.h"

#define TX_QUEUE_LEN  64
#define SEND_WAIT_MS  20  // the TX task runs below the LCC task; let it catch up
#define LINE_MAX      48

static QueueHandle_t tx_queue;
static TaskHandle_t rx_task;

// Runs in the SDK's low-priority USB interrupt
static void chars_available(void *param)
{
    (void)param;
    if (rx_task) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(rx_task, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

void usb_gc_init(void)
{
    tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(lcc_frame_t));
}

bool usb_gc_send(const lcc_frame_t *frame)
{
    return xQueueSend(tx_queue, frame, pdMS_TO_TICKS(SEND_WAIT_MS)) == pdTRUE;
}

bool usb_gc_send_from_isr(const lcc_frame_t *frame, BaseType_t *woken)
{
    return xQueueSendFromISR(tx_queue, frame, woken) == pdTRUE;
}

void usb_console_printf(const char *fmt, ...)
{
    char buf[96];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    if (n > 0)
        stdio_put_string(buf, n, false, true);
}

// ------------------------------------------------------------------
// Console
// ------------------------------------------------------------------

static uint64_t pending_node_id;

static void show_node_id(uint32_t arg)
{
    (void)arg;
    app_show_node_id();
}

static void set_node_id(uint32_t arg)
{
    (void)arg;
    app_set_node_id(pending_node_id);
}

static void handle_line(char *line)
{
    if (strncmp(line, "nodeid", 6) == 0 && (line[6] == 0 || line[6] == ' ')) {
        const char *arg = line + 6;
        while (*arg == ' ')
            arg++;
        if (*arg == 0) {
            lcc_task_call(show_node_id, 0);
        } else if (node_id_parse(arg, &pending_node_id)) {
            lcc_task_call(set_node_id, 0);
        } else {
            usb_console_printf("nodeid: expected 12 hex digits, e.g. nodeid 05.01.01.01.12.34\n");
        }
    } else if (strcmp(line, "help") == 0) {
        usb_console_printf("commands: nodeid, nodeid <id>\n");
    } else {
        usb_console_printf("unknown command '%s' (try help)\n", line);
    }
}

// ------------------------------------------------------------------
// Tasks
// ------------------------------------------------------------------

void usb_gc_rx_task(void *param)
{
    (void)param;
    rx_task = xTaskGetCurrentTaskHandle();
    stdio_set_chars_available_callback(chars_available, NULL);

    lcc_gc_parser_t parser = {0};
    char line[LINE_MAX];
    int line_len = 0;

    for (;;) {
        int c;
        while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
            lcc_frame_t frame;
            if (lcc_gc_parse(&parser, (char)c, &frame)) {
                lcc_task_post_frame(&frame);
                can_bus_send(&frame);
                continue;
            }
            if (!lcc_gc_idle(&parser)) {
                line_len = 0;  // inside a GridConnect frame
                continue;
            }

            if (c == '\n' || c == '\r') {
                line[line_len] = 0;
                if (line_len > 0 && isalpha((unsigned char)line[0]))
                    handle_line(line);
                line_len = 0;
            } else if (line_len < LINE_MAX - 1) {
                line[line_len++] = (char)c;
            }
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

void usb_gc_tx_task(void *param)
{
    (void)param;
    for (;;) {
        lcc_frame_t frame;
        xQueueReceive(tx_queue, &frame, portMAX_DELAY);
        if (!stdio_usb_connected())
            continue;

        char buf[LCC_GC_MAX_LEN];
        int n = lcc_gc_encode(&frame, buf, sizeof(buf));
        if (n > 0)
            stdio_put_string(buf, n, false, false);
    }
}
