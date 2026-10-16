#ifndef TURNOUTS_H
#define TURNOUTS_H

#include <stdint.h>
#include "config_layout.h"

// Servo switch machines: LCC throw/close/center commands, thrown/closed
// feedback, frog polarity and fascia toggles.

void turnouts_defaults(config_layout_t *cfg, uint64_t node_id);

// Before the scheduler starts: move servos to their power-on state and
// register their events.
void turnouts_init(void);

void turnouts_config_written(void);

// A fascia input (1-4) was pressed.
void turnouts_input_pressed(int input);

// Default event IDs are derived from the node ID; move any still at their
// default from old_id's range to new_id's.
void turnouts_migrate_events(config_layout_t *cfg, uint64_t old_id, uint64_t new_id);

#endif // TURNOUTS_H
