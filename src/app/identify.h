#ifndef IDENTIFY_H
#define IDENTIFY_H

#include "config_layout.h"

// Identify LED: blinks while enabled in the configuration, until its
// timeout clears the setting again.

void identify_defaults(config_layout_t *cfg);
void identify_init(void);

// The identify settings changed.
void identify_apply(void);

// Another node asked about this one: restart the timeout if blinking.
void identify_requested(void);

#endif // IDENTIFY_H
