#ifndef DBG_H
#define DBG_H

#include <stdio.h>
#include "SEGGER_RTT.h"

// Debug print via RTT only (does not go to USB CDC / UART)
#define DBG(...) do { \
    char _dbg_buf[128]; \
    int _dbg_n = snprintf(_dbg_buf, sizeof(_dbg_buf), __VA_ARGS__); \
    if (_dbg_n > 0) SEGGER_RTT_Write(0, _dbg_buf, (unsigned)_dbg_n); \
} while (0)

#endif // DBG_H
