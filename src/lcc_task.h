#ifndef LCC_TASK_H
#define LCC_TASK_H

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "lcc/can.h"

// The LCC task owns the LCC library, the configuration and the application
// state. Other tasks and interrupts hand it work through a queue: received
// frames, or a function to run in the task.

// Before the scheduler starts: create the queue and set up the library.
void lcc_task_init(uint64_t node_id);

void lcc_task(void *param);

bool lcc_task_post_frame(const lcc_frame_t *frame);
bool lcc_task_post_frame_from_isr(const lcc_frame_t *frame, BaseType_t *woken);

typedef void (*lcc_task_fn_t)(uint32_t arg);

// Run fn(arg) in the LCC task. Returns false if the queue is full.
bool lcc_task_call(lcc_task_fn_t fn, uint32_t arg);
bool lcc_task_call_from_isr(lcc_task_fn_t fn, uint32_t arg, BaseType_t *woken);

// From any task: reboot through the LCC library, which releases the alias
// first and reboots 100 ms later (lcc_reboot).
void lcc_task_reboot(void);

// (Re)start the hardware watchdog the LCC task feeds.
void lcc_task_start_watchdog(void);

#endif // LCC_TASK_H
