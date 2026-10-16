#ifndef LCC_GRIDCONNECT_H
#define LCC_GRIDCONNECT_H

#include <stdbool.h>
#include <stdint.h>
#include "lcc/can.h"

/*
 * GridConnect ASCII framing for CAN over a serial link:
 *   :X<8 hex digit CAN ID>N<0-16 hex digit data>;
 */

/* ":X" + 8 + "N" + 16 + ";\n" + NUL */
#define LCC_GC_MAX_LEN  30

typedef struct {
    uint8_t  state;
    uint8_t  nibbles;
    uint32_t id;
    uint8_t  data[8];
} lcc_gc_parser_t;

/* Encode a frame followed by "\n". Returns the length, or -1 if buf is
 * smaller than LCC_GC_MAX_LEN. */
int lcc_gc_encode(const lcc_frame_t *frame, char *buf, int buf_size);

/* Feed one character; returns true when *frame holds a complete frame. */
bool lcc_gc_parse(lcc_gc_parser_t *parser, char c, lcc_frame_t *frame);

/* True while the parser is between frames, i.e. c was not part of one. */
bool lcc_gc_idle(const lcc_gc_parser_t *parser);

#endif /* LCC_GRIDCONNECT_H */
