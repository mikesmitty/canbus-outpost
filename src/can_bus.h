#ifndef CAN_BUS_H
#define CAN_BUS_H

#include <stdbool.h>
#include "lcc/can.h"

// The LCC CAN bus (can2040 on PIO1). Received frames go to the LCC task and
// the USB GridConnect bridge.

// Before the scheduler starts
void can_bus_init(void);

// Starts can2040 and transmits queued frames, waiting for room in can2040's
// transmit queue when it is full.
void can_bus_task(void *param);

// Queue a frame for transmission, waiting up to 20 ms for room. Returns
// false (and drops it) if the queue stays full, e.g. with no other nodes.
bool can_bus_send(const lcc_frame_t *frame);

#endif // CAN_BUS_H
