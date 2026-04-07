#include "dbg.h"
#include <stdarg.h>
#include <stdio.h>
#include "SEGGER_RTT.h"
#include "hardware/sync.h"

static char dbg_buffer[256];

void dbg_printf(const char *fmt, ...) {
    uint32_t ints = save_and_disable_interrupts();
    
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(dbg_buffer, sizeof(dbg_buffer), fmt, args);
    va_end(args);
    
    if (n > 0) {
        SEGGER_RTT_Write(0, dbg_buffer, (unsigned)n);
    }
    
    restore_interrupts(ints);
}
