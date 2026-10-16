# lcc

A small OpenLCB/LCC node library in C11 for microcontrollers. It has no
dependencies on an SDK, an RTOS or the heap. Frames go out through a callback,
time comes in as a millisecond counter, and every call happens on one thread.
That makes it straightforward to run under FreeRTOS, bare metal or a host test.

## What it covers

| Area | Details |
|---|---|
| CAN frame transfer | Alias allocation (CID/RID/AMD), defending the alias, duplicate-alias detection and recovery (AMR), Alias Map Enquiry |
| Message network | Initialization Complete, Verify Node ID (global and addressed), Optional Interaction Rejected for unsupported addressed messages |
| Protocol Support | Reply built from what is registered |
| SNIP | Manufacturer and user sections |
| Event transport | Producer/consumer registry, identify replies with valid/invalid/unknown state, event reports, announcement after initialization, producer ranges (Producer Range Identified, application-answered identify for IDs in the range) |
| Datagrams | Reassembly with timeout, busy and out-of-order rejection, OK with reply-pending, outgoing queue with resend on temporary rejection |
| Memory configuration | Read and write on registered spaces, space information, configuration options, lock, Update Complete, reboot, factory reset |
| Firmware upgrade | Freeze/Unfreeze, space 0xEF written by datagram (the application stores and checks the image), Protocol Support bits |
| GridConnect | Encoder and parser |

It does not cover streams, traction or nodes with more than one virtual
node.

## Using it

```c
#include "lcc/node.h"
#include "lcc/events.h"
#include "lcc/memconfig.h"

static bool send(const lcc_frame_t *f) { return can_queue_frame(f); }

lcc_node_config_t cfg = {
    .node_id = 0x050101011234,
    .manufacturer = "me", .model = "widget",
    .hardware_version = "1", .software_version = "1.0",
    .user_name = (const char *)config.user_name,
    .user_description = (const char *)config.user_description,
    .hooks = { .send = send, .config_update_complete = save_config },
};
lcc_init(&cfg);

// Registration happens after lcc_init and before lcc_start.
static const lcc_event_t throw_cmd = {
    .id = config.throw_event,   // 8 bytes, big-endian, read on every use
    .on_event = on_throw,
};
lcc_event_register(&throw_cmd);

static const lcc_memspace_t cdi = { .space = LCC_SPACE_CDI, .size = sizeof(cdi_xml),
                                    .data = (const uint8_t *)cdi_xml };
lcc_memconfig_add_space(&cdi);

lcc_start(now_ms());

// Event loop: feed received frames and run timers when they come due.
for (;;) {
    wait_for_frame_or_timeout(lcc_ms_until_next_timer(now_ms()));
    if (have_frame)
        lcc_frame_received(&frame, now_ms());
    lcc_run_timers(now_ms());
}
```

Configuration spaces are byte arrays in OpenLCB byte order (big-endian).
Configuration tools read and write them as-is, so the application reads
values with the `lcc/bytes.h` helpers.

## Tests

`make -C test` builds and runs the host tests with the system C compiler.
