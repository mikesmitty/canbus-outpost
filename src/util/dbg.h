#ifndef DBG_H
#define DBG_H

// Debug print via RTT only (does not go to USB CDC / UART)
void dbg_printf(const char *fmt, ...);

#define DBG(...) dbg_printf(__VA_ARGS__)

#endif // DBG_H
