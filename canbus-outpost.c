#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_rtt.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app/app.h"
#include "app/version.h"
#include "can_bus.h"
#include "lcc_task.h"
#include "node_id.h"
#include "railcom.h"
#include "servo.h"
#include "usb_gridconnect.h"
#include "util/dbg.h"

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
/* Diagnostic HardFault handler — prints state to RTT before halting. */
/*                                                                    */
/* Default SDK behaviour calls panic()/__breakpoint(), which is a     */
/* silent no-op when no debugger has DEBUGEN set, so faults present   */
/* as a freeze with zero log. This handler captures the auto-stacked  */
/* exception frame and the SCB fault-status registers and prints them */
/* over RTT before parking in an infinite loop.                       */
/* ------------------------------------------------------------------ */

static void __attribute__((used)) fault_handler_c(uint32_t *stack)
{
    uint32_t r0  = stack[0];
    uint32_t r1  = stack[1];
    uint32_t r2  = stack[2];
    uint32_t r3  = stack[3];
    uint32_t r12 = stack[4];
    uint32_t lr  = stack[5];
    uint32_t pc  = stack[6];
    uint32_t psr = stack[7];

    uint32_t cfsr  = *(volatile uint32_t *)0xE000ED28u;
    uint32_t hfsr  = *(volatile uint32_t *)0xE000ED2Cu;
    uint32_t mmfar = *(volatile uint32_t *)0xE000ED34u;
    uint32_t bfar  = *(volatile uint32_t *)0xE000ED38u;
    uint32_t shcsr = *(volatile uint32_t *)0xE000ED24u;

    DBG("\n!!! HardFault on core %u !!!\n", (unsigned)get_core_num());
    DBG("  PC=%08lx  LR=%08lx  PSR=%08lx\n",
        (unsigned long)pc, (unsigned long)lr, (unsigned long)psr);
    DBG("  R0=%08lx R1=%08lx R2=%08lx R3=%08lx R12=%08lx\n",
        (unsigned long)r0, (unsigned long)r1, (unsigned long)r2,
        (unsigned long)r3, (unsigned long)r12);
    DBG("  CFSR=%08lx HFSR=%08lx SHCSR=%08lx\n",
        (unsigned long)cfsr, (unsigned long)hfsr, (unsigned long)shcsr);
    DBG("  MMFAR=%08lx BFAR=%08lx\n",
        (unsigned long)mmfar, (unsigned long)bfar);

    while (1) { tight_loop_contents(); }
}

void __attribute__((naked, used)) isr_hardfault(void)
{
    __asm volatile (
        "tst lr, #4         \n"
        "ite eq             \n"
        "mrseq r0, msp      \n"
        "mrsne r0, psp      \n"
        "b fault_handler_c  \n"
    );
}

/* ------------------------------------------------------------------ */
/* Malloc failed hook                                                 */
/* ------------------------------------------------------------------ */

void vApplicationMallocFailedHook(void)
{
    DBG("FATAL: FreeRTOS malloc failed!\n");
    __breakpoint();
    while (1) {}
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    stdio_init_all();
    /* RTT carries the debug log (DBG); keep GridConnect traffic off it */
    stdio_set_driver_enabled(&stdio_rtt, false);

    DBG("canbus-outpost %s starting\n", FIRMWARE_SW_VERSION);

    bool assigned;
    uint64_t node_id = node_id_load(&assigned);
    char id[18];
    node_id_format(node_id, id);
    DBG("Node ID %s%s\n", id,
        assigned ? "" : " (temporary: set one with the 'nodeid' USB command)");

    servo_init();
    lcc_task_init(node_id);
    app_init(node_id, assigned);
    railcom_init();
    can_bus_init();
    usb_gc_init();

    xTaskCreate(lcc_task, "lcc", 2048, NULL, 2, NULL);
    xTaskCreate(can_bus_task, "can", 1024, NULL, 3, NULL);
    xTaskCreate(usb_gc_rx_task, "usb_rx", 1024, NULL, 1, NULL);
    xTaskCreate(usb_gc_tx_task, "usb_tx", 1024, NULL, 1, NULL);
    /* Highest priority: railcom_task must copy each cutout out of the
     * 4 ms DMA ring before it is overwritten. */
    xTaskCreate(railcom_task, "railcom", 2048, NULL, configMAX_PRIORITIES - 1, NULL);

    vTaskStartScheduler();
    return 0;
}
