#ifndef SIGNALS_H
#define SIGNALS_H

#include <stdint.h>
#include "config_layout.h"

// Signal heads on the WS2811 chain: LCC events choose each head's aspect.

void signals_defaults(config_layout_t *cfg, uint64_t node_id);

// Before the scheduler starts: show each head's power-on aspect and register
// its events.
void signals_init(void);

void signals_config_written(void);

// Default event IDs are derived from the node ID; move any still at their
// default from old_id's range to new_id's.
void signals_migrate_events(config_layout_t *cfg, uint64_t old_id, uint64_t new_id);

#endif // SIGNALS_H
