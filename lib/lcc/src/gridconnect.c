#include "lcc/gridconnect.h"

enum {
    GC_IDLE,
    GC_TYPE,   /* after ':', expecting 'X' */
    GC_ID,     /* hex ID digits, then 'N' */
    GC_DATA,   /* hex data digits, then ';' */
};

static const char hex_digits[] = "0123456789ABCDEF";

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int lcc_gc_encode(const lcc_frame_t *frame, char *buf, int buf_size)
{
    if (buf_size < LCC_GC_MAX_LEN)
        return -1;

    char *p = buf;
    *p++ = ':';
    *p++ = 'X';
    for (int i = 7; i >= 0; i--)
        *p++ = hex_digits[(frame->id >> (i * 4)) & 0xF];
    *p++ = 'N';
    for (int i = 0; i < frame->dlc && i < 8; i++) {
        *p++ = hex_digits[frame->data[i] >> 4];
        *p++ = hex_digits[frame->data[i] & 0xF];
    }
    *p++ = ';';
    *p++ = '\n';
    *p = '\0';
    return (int)(p - buf);
}

bool lcc_gc_parse(lcc_gc_parser_t *p, char c, lcc_frame_t *frame)
{
    switch (p->state) {
    case GC_IDLE:
        if (c == ':') {
            p->id = 0;
            p->nibbles = 0;
            p->state = GC_TYPE;
        }
        return false;

    case GC_TYPE:
        p->state = (c == 'X' || c == 'x') ? GC_ID : GC_IDLE;
        return false;

    case GC_ID:
        if (c == 'N' || c == 'n') {
            p->nibbles = 0;
            p->state = GC_DATA;
        } else {
            int v = hex_value(c);
            if (v < 0)
                p->state = GC_IDLE;
            else
                p->id = (p->id << 4) | (uint32_t)v;
        }
        return false;

    case GC_DATA:
        if (c == ';') {
            p->state = GC_IDLE;
            if (p->nibbles & 1)
                return false;  /* odd number of data digits */
            frame->id = p->id & 0x1FFFFFFF;
            frame->dlc = p->nibbles / 2;
            for (int i = 0; i < frame->dlc; i++)
                frame->data[i] = p->data[i];
            return true;
        } else {
            int v = hex_value(c);
            if (v < 0 || p->nibbles >= 16) {
                p->state = GC_IDLE;
                return false;
            }
            uint8_t i = p->nibbles / 2;
            if (p->nibbles & 1)
                p->data[i] |= (uint8_t)v;
            else
                p->data[i] = (uint8_t)(v << 4);
            p->nibbles++;
        }
        return false;

    default:
        p->state = GC_IDLE;
        return false;
    }
}

bool lcc_gc_idle(const lcc_gc_parser_t *p)
{
    return p->state == GC_IDLE;
}
