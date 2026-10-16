#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "config_layout.h"

// The configuration memory space, shared with LCC configuration tools.
// Owned by the LCC task: only touch it from there.
extern config_layout_t config;

// Restore the saved configuration. Returns how many bytes were restored:
// 0 if there is none (or it has an incompatible version), less than
// sizeof(config) if it predates fields added at the end.
size_t config_load(void);

bool config_save(void);

#endif // CONFIG_H
