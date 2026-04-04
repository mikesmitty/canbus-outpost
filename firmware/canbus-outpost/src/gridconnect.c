#include "gridconnect.h"

/*
 * GridConnect ASCII CAN frame codec.
 * Wire format: :X<8 hex CAN ID>N<data hex>;\n
 */

enum {
    GC_STATE_IDLE,
    GC_STATE_TYPE,      /* after ':', expecting 'X' */
    GC_STATE_ID,        /* reading hex ID digits */
    GC_STATE_SEP,       /* expecting 'N' separator */
    GC_STATE_DATA,      /* reading hex data digits */
};

static const char hex_chars[] = "0123456789ABCDEF";

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int gc_encode(const lcc_frame_t *frame, char *buf, int buf_size)
{
    if (buf_size < GC_MAX_LEN)
        return -1;

    char *p = buf;

    *p++ = ':';
    *p++ = 'X';

    /* 8 hex digits for 29-bit CAN ID (zero-padded) */
    for (int i = 7; i >= 0; i--)
        *p++ = hex_chars[(frame->id >> (i * 4)) & 0xF];

    *p++ = 'N';

    /* Data bytes as hex pairs */
    for (int i = 0; i < frame->dlc && i < 8; i++) {
        *p++ = hex_chars[(frame->data[i] >> 4) & 0xF];
        *p++ = hex_chars[frame->data[i] & 0xF];
    }

    *p++ = ';';
    *p++ = '\n';
    *p = '\0';

    return (int)(p - buf);
}

bool gc_parse_byte(gc_parser_t *p, char c, lcc_frame_t *frame)
{
    switch (p->state) {
    case GC_STATE_IDLE:
        if (c == ':') {
            p->id = 0;
            p->dlc = 0;
            p->nibble = 0;
            p->state = GC_STATE_TYPE;
        }
        break;

    case GC_STATE_TYPE:
        if (c == 'X' || c == 'x') {
            p->state = GC_STATE_ID;
        } else {
            p->state = GC_STATE_IDLE;
        }
        break;

    case GC_STATE_ID:
        if (c == 'N' || c == 'n') {
            p->state = GC_STATE_DATA;
        } else {
            int v = hex_val(c);
            if (v >= 0) {
                p->id = (p->id << 4) | (uint32_t)v;
            } else {
                p->state = GC_STATE_IDLE;
            }
        }
        break;

    case GC_STATE_DATA:
        if (c == ';') {
            /* Complete frame */
            frame->id = p->id & 0x1FFFFFFF;
            frame->dlc = p->dlc;
            for (int i = 0; i < p->dlc; i++)
                frame->data[i] = p->data[i];
            p->state = GC_STATE_IDLE;
            return true;
        } else {
            int v = hex_val(c);
            if (v >= 0 && p->dlc < 8) {
                if (!(p->nibble & 1)) {
                    p->data[p->dlc] = (uint8_t)(v << 4);
                } else {
                    p->data[p->dlc] |= (uint8_t)v;
                    p->dlc++;
                }
                p->nibble++;
            } else if (v < 0) {
                p->state = GC_STATE_IDLE;
            }
        }
        break;

    default:
        p->state = GC_STATE_IDLE;
        break;
    }

    return false;
}
