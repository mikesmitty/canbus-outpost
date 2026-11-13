// Stand-in for the SDK's flash-safety header: the function runs at once
#pragma once
#include <stdint.h>

#define PICO_OK 0

static inline int flash_safe_execute(void (*func)(void *), void *param, uint32_t timeout_ms)
{
    (void)timeout_ms;
    func(param);
    return PICO_OK;
}
