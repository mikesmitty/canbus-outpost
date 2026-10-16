#ifndef NODE_ID_H
#define NODE_ID_H

#include <stdbool.h>
#include <stdint.h>

// The node's LCC node ID. It should come from a range assigned by OpenLCB
// (free to request) and is stored once per board, separate from the
// configuration so a factory reset keeps it. Until one is stored, a
// locally-allocated ID (0A.xx.xx.xx.xx.xx) derived from the chip's device
// ID stands in: it can't clash with other vendors' nodes, but isn't fit for
// a board that moves between layouts.

// Returns the stored ID, or the stand-in with *assigned = false.
uint64_t node_id_load(bool *assigned);

bool node_id_store(uint64_t node_id);

// Parse "05.01.01.01.12.34", "05:01:01:01:12:34" or "050101011234".
bool node_id_parse(const char *s, uint64_t *node_id);

// Format as "05.01.01.01.12.34" into buf (at least 18 bytes).
void node_id_format(uint64_t node_id, char *buf);

#endif // NODE_ID_H
