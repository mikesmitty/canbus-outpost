#ifndef GRIDCONNECT_H
#define GRIDCONNECT_H

#include <stdint.h>
#include <stdbool.h>
#include "lcc_defs.h"

/*
 * GridConnect ASCII CAN frame codec.
 * Format: :X<8 hex CAN ID>N<data hex>;\n
 * Reference: OpenMRN GridConnect.hxx
 */

/* Maximum encoded frame: ":X" + 8 id + "N" + 16 data + ";\n" + NUL = 29 */
#define GC_MAX_LEN  30

/* Parser states */
typedef struct {
    uint8_t  state;
    uint32_t id;
    uint8_t  data[8];
    uint8_t  dlc;
    uint8_t  nibble;    /* stashed high nibble for data byte assembly */
} gc_parser_t;

/* Encode an lcc_frame_t into a GridConnect ASCII string.
 * Returns number of bytes written (excluding NUL), or -1 on error. */
int gc_encode(const lcc_frame_t *frame, char *buf, int buf_size);

/* Feed one character to the parser.
 * Returns true when a complete frame has been parsed into *frame. */
bool gc_parse_byte(gc_parser_t *p, char c, lcc_frame_t *frame);

#endif /* GRIDCONNECT_H */
