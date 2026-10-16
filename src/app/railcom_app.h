#ifndef RAILCOM_APP_H
#define RAILCOM_APP_H

#include <stdint.h>
#include "config_layout.h"

// RailCom detection as LCC events, per track:
// - an Occupied/Clear event pair, for JMRI sensors and other nodes
// - locomotive reports in the OpenLCB DCC detector format, for JMRI
//   reporters: a 64K event range per track whose low 16 bits say which
//   locomotive entered (and which way it faces) or left
//
// railcom.c reports from its own task; the reports are handed to the LCC
// task, where the rest of this runs.

void railcom_app_defaults(config_layout_t *cfg, uint64_t node_id);

// Before the scheduler starts: register the tracks' events.
void railcom_app_init(void);

void railcom_app_config_written(void);

// Default event IDs and reporter IDs are derived from the node ID; move any
// still at their default from old_id's to new_id's.
void railcom_app_migrate_events(config_layout_t *cfg, uint64_t old_id, uint64_t new_id);

#endif // RAILCOM_APP_H
