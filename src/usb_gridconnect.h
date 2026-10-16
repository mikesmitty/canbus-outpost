#ifndef USB_GRIDCONNECT_H
#define USB_GRIDCONNECT_H

#include <stdbool.h>
#include "FreeRTOS.h"
#include "lcc/can.h"

// USB CDC bridge to a host (JMRI and similar) using GridConnect framing.
// Frames from the host go onto the CAN bus and to the LCC task; frames from
// the bus and from this node go to the host. Lines that start with a letter
// are console commands:
//   nodeid                     show the node ID
//   nodeid 05.01.01.01.12.34   store a node ID and reboot

// Before the scheduler starts
void usb_gc_init(void);

void usb_gc_rx_task(void *param);
void usb_gc_tx_task(void *param);

bool usb_gc_send(const lcc_frame_t *frame);
bool usb_gc_send_from_isr(const lcc_frame_t *frame, BaseType_t *woken);

// Console output, written as one piece so it never splits a frame
void usb_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif // USB_GRIDCONNECT_H
