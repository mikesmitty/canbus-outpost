#ifndef NODE_ID_H
#define NODE_ID_H

#include <stdbool.h>
#include <stdint.h>

// The node's LCC node ID. It must come from a range assigned by OpenLCB
// (free to request) and is stored once per board, separate from the
// configuration so a factory reset keeps it. Until one is stored, an ID
// derived from the chip's serial number stands in, which is not guaranteed
// unique on a layout.

// Returns the stored ID, or the stand-in with *assigned = false.
uint64_t node_id_load(bool *assigned);

bool node_id_store(uint64_t node_id);

// Parse "05.01.01.01.12.34", "05:01:01:01:12:34" or "050101011234".
bool node_id_parse(const char *s, uint64_t *node_id);

// Format as "05.01.01.01.12.34" into buf (at least 18 bytes).
void node_id_format(uint64_t node_id, char *buf);

#endif // NODE_ID_H
