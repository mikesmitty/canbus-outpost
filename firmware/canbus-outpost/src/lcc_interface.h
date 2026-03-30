#ifndef LCC_INTERFACE_H
#define LCC_INTERFACE_H

#include "openlcb/openlcb_node.h"
#include "drivers/canbus/can_types.h"

void lcc_interface_init(void);
void lcc_interface_run(void);
void lcc_interface_100ms_tick(void);

#endif // LCC_INTERFACE_H
