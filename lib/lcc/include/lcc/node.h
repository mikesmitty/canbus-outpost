#ifndef LCC_NODE_H
#define LCC_NODE_H

#include <stdbool.h>
#include <stdint.h>
#include "lcc/can.h"

/*
 * OpenLCB node over CAN: alias allocation and the basic node protocols
 * (Verify Node ID, Protocol Support, Simple Node Information).
 *
 * The library runs one node. It never blocks, allocates or touches
 * hardware: frames go out through hooks.send, time comes in as a
 * millisecond counter, and all lcc_* calls must come from one thread.
 */

#define LCC_NO_TIMER  UINT32_MAX

/* Callbacks into the application. Only send is required. */
typedef struct {
    /* Queue a frame for transmission. May wait briefly for room (replies
     * can run to dozens of frames) but must not block indefinitely. */
    bool (*send)(const lcc_frame_t *frame);
    /* The node claimed its alias and sent Initialization Complete. */
    void (*initialized)(void);
    /* Another node asked about this one (Verify Node ID, SNIP request). */
    void (*identify_requested)(void);
    /* Memory configuration: the configuration tool finished writing. */
    void (*config_update_complete)(void);
    /* Memory configuration: confirmed factory reset. A reboot follows. */
    void (*factory_reset)(void);
    /* Reboot now: after a memory configuration Reboot, factory reset or
     * Unfreeze of a good image, or lcc_reboot. The alias is released. */
    void (*reboot)(void);
    /* Another node announced this node's ID (once per start). The library
     * has sent the Duplicate Node ID Detected event; the application
     * should show it to the user. */
    void (*duplicate_node_id)(void);
} lcc_hooks_t;

typedef struct {
    uint64_t node_id;
    /* Simple Node Information, manufacturer section */
    const char *manufacturer;
    const char *model;
    const char *hardware_version;
    const char *software_version;
    /* Simple Node Information, user section: read on every request, so
     * these can point at configuration memory. At most 63 characters of
     * each are sent; they need not be NUL-terminated past that. */
    const char *user_name;
    const char *user_description;
    lcc_hooks_t hooks;
} lcc_node_config_t;

/* Set up the node and its protocol modules. Register events, datagram
 * handlers and memory spaces after this and before lcc_start. */
void lcc_init(const lcc_node_config_t *config);

/* Begin alias allocation. */
void lcc_start(uint32_t now_ms);

/* Feed every frame received from the bus (and from any bridged host). */
void lcc_frame_received(const lcc_frame_t *frame, uint32_t now_ms);

/* Run expired timers. Call when lcc_ms_until_next_timer() elapses. */
void lcc_run_timers(uint32_t now_ms);

/* Milliseconds until lcc_run_timers needs calling, or LCC_NO_TIMER. */
uint32_t lcc_ms_until_next_timer(uint32_t now_ms);

/* Reboot through the reboot hook. The alias is released first (Alias Map
 * Reset) and the node goes silent; the hook runs 100 ms later, so the
 * frames already queued can go out. Does nothing without the hook. */
void lcc_reboot(uint32_t now_ms);

/* True once the alias is claimed and Initialization Complete is sent. */
bool lcc_is_initialized(void);
uint16_t lcc_alias(void);
uint64_t lcc_node_id(void);

#endif /* LCC_NODE_H */
