#include "dbg.h"
#include <stdarg.h>
#include <stdio.h>
#include "SEGGER_RTT.h"

void dbg_printf(const char *fmt, ...) {
    // Format on the caller's stack so no lock is held while formatting;
    // SEGGER_RTT_Write locks internally and only masks interrupts below the
    // CAN interrupt's priority.
    char buf[160];

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    if (n > 0)
        SEGGER_RTT_Write(0, buf, (unsigned)n);
}
