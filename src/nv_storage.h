#ifndef NV_STORAGE_H
#define NV_STORAGE_H

#include <stddef.h>
#include <stdbool.h>

bool nv_storage_init(void *buffer, size_t size);
bool nv_storage_write(const void *buffer, size_t size);

#endif // NV_STORAGE_H
