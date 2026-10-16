#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

// The node's application: switch machines, fascia inputs, identify LED and
// RailCom reporting, on top of the LCC library. Everything here runs in
// the LCC task unless noted.

// Before the scheduler starts: load configuration, set outputs to their
// power-on state and register LCC events.
void app_init(uint64_t node_id, bool node_id_assigned);

// At the start of the LCC task, before alias allocation.
void app_start(void);

// LCC memory configuration wrote to the configuration space.
void app_config_written(uint32_t address, uint32_t length);

// LCC memory configuration factory reset: back to defaults and save.
void app_factory_reset(void);

// Another node asked about this one (blink the identify LED if enabled).
void app_identify_requested(void);

// Another node is using this node's ID: tell the user (log, USB console,
// fast-blinking identify LED).
void app_duplicate_node_id(void);

// USB console commands
void app_show_node_id(void);
void app_set_node_id(uint64_t node_id);  // stores it and reboots

#endif // APP_H
