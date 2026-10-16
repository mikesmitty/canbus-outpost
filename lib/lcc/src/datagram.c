#include <string.h>
#include "internal.h"
#include "lcc/bytes.h"
#include "lcc/datagram.h"

#define REPLY_PENDING_FLAG  0x80

#define RX_TIMEOUT_MS       3000  /* drop a half-received datagram */
#define TX_ACK_TIMEOUT_MS   3000  /* give up waiting for OK/Rejected */
#define TX_RESEND_DELAY_MS  200
#define TX_MAX_TRIES        4
#define TX_QUEUE_LEN        2

static const lcc_datagram_handler_t *handlers[LCC_MAX_DATAGRAM_HANDLERS];
static int handler_count;

/* Reassembly of one incoming multi-frame datagram */
static struct {
    bool active;
    uint16_t src;
    uint8_t len;
    uint8_t buf[LCC_DATAGRAM_MAX];
    uint32_t deadline;
    /* One other sender we turned away while busy; rejected at its final frame */
    bool refusing;
    uint16_t refused_src;
} rx;

/* Outgoing datagrams; the head is in flight */
typedef struct {
    uint16_t dst;
    uint8_t len;
    uint8_t buf[LCC_DATAGRAM_MAX];
} tx_entry_t;

static struct {
    tx_entry_t queue[TX_QUEUE_LEN];
    int count;
    bool in_flight;      /* sent, waiting for OK/Rejected */
    bool resend_wait;    /* temporarily rejected, waiting to resend */
    uint8_t tries;
    uint32_t deadline;   /* ack timeout or resend time */
} tx;

/* ------------------------------------------------------------------ */
/* Replies                                                            */
/* ------------------------------------------------------------------ */

static void send_ok(uint16_t dst, bool reply_pending)
{
    uint8_t flags = reply_pending ? REPLY_PENDING_FLAG : 0;
    lcc_send_addressed(LCC_MTI_DATAGRAM_OK, dst, &flags, 1);
}

static void send_rejected(uint16_t dst, uint16_t code)
{
    uint8_t buf[2];
    lcc_put_be16(buf, code);
    lcc_send_addressed(LCC_MTI_DATAGRAM_REJECTED, dst, buf, 2);
}

static void deliver(uint16_t src, const uint8_t *buf, uint8_t len)
{
    if (len == 0) {
        send_rejected(src, LCC_ERROR_INVALID_ARGS);
        return;
    }

    for (int i = 0; i < handler_count; i++) {
        const lcc_datagram_handler_t *h = handlers[i];
        if (h->content_type != buf[0])
            continue;

        bool reply_pending = false;
        uint16_t code = h->accept(src, buf, len, &reply_pending);
        if (code) {
            send_rejected(src, code);
            return;
        }
        send_ok(src, reply_pending);
        if (h->process)
            h->process(src, buf, len);
        return;
    }
    send_rejected(src, LCC_ERROR_UNIMPLEMENTED_CMD);
}

/* ------------------------------------------------------------------ */
/* Receiving                                                          */
/* ------------------------------------------------------------------ */

void lcc_datagram_frame(const lcc_frame_t *f, uint32_t now)
{
    uint16_t src = lcc_frame_src(f->id);
    bool ours = rx.active && rx.src == src;

    switch (lcc_can_frame_type(f->id)) {
    case LCC_FRAME_DATAGRAM_ONE:
        deliver(src, f->data, f->dlc);
        return;

    case LCC_FRAME_DATAGRAM_FIRST:
        if (rx.active && !ours) {
            rx.refusing = true;
            rx.refused_src = src;
            return;
        }
        rx.active = true;
        rx.src = src;
        rx.len = f->dlc;
        memcpy(rx.buf, f->data, f->dlc);
        rx.deadline = now + RX_TIMEOUT_MS;
        return;

    case LCC_FRAME_DATAGRAM_MIDDLE:
    case LCC_FRAME_DATAGRAM_FINAL:
        break;

    default:
        return;
    }

    bool final = lcc_can_frame_type(f->id) == LCC_FRAME_DATAGRAM_FINAL;

    if (!ours) {
        if (!final)
            return;
        if (rx.refusing && rx.refused_src == src) {
            rx.refusing = false;
            send_rejected(src, LCC_ERROR_BUFFER_UNAVAILABLE);
        } else {
            send_rejected(src, LCC_ERROR_OUT_OF_ORDER);
        }
        return;
    }

    if (rx.len + f->dlc > LCC_DATAGRAM_MAX) {
        rx.active = false;
        send_rejected(src, LCC_ERROR_INVALID_ARGS);
        return;
    }
    memcpy(rx.buf + rx.len, f->data, f->dlc);
    rx.len += f->dlc;
    rx.deadline = now + RX_TIMEOUT_MS;

    if (final) {
        rx.active = false;
        deliver(src, rx.buf, rx.len);
    }
}

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

static void transmit_head(uint32_t now)
{
    const tx_entry_t *e = &tx.queue[0];
    uint16_t alias = lcc_alias();

    if (e->len <= 8) {
        lcc_frame_t f = { .id = lcc_datagram_id(LCC_FRAME_DATAGRAM_ONE, e->dst, alias),
                          .dlc = e->len };
        memcpy(f.data, e->buf, e->len);
        lcc_send_frame(&f);
    } else {
        for (uint8_t off = 0; off < e->len; off += 8) {
            uint8_t chunk = (uint8_t)(e->len - off > 8 ? 8 : e->len - off);
            uint8_t type = off == 0 ? LCC_FRAME_DATAGRAM_FIRST :
                           off + chunk >= e->len ? LCC_FRAME_DATAGRAM_FINAL :
                                                   LCC_FRAME_DATAGRAM_MIDDLE;
            lcc_frame_t f = { .id = lcc_datagram_id(type, e->dst, alias), .dlc = chunk };
            memcpy(f.data, e->buf + off, chunk);
            lcc_send_frame(&f);
        }
    }

    tx.tries++;
    tx.in_flight = true;
    tx.resend_wait = false;
    tx.deadline = now + TX_ACK_TIMEOUT_MS;
}

static void pop_head(uint32_t now)
{
    if (tx.count > 0) {
        memmove(&tx.queue[0], &tx.queue[1], sizeof(tx.queue[0]) * (TX_QUEUE_LEN - 1));
        tx.count--;
    }
    tx.in_flight = false;
    tx.resend_wait = false;
    tx.tries = 0;
    if (tx.count > 0)
        transmit_head(now);
}

bool lcc_datagram_send(uint16_t dst_alias, const uint8_t *data, uint8_t len)
{
    if (len == 0 || len > LCC_DATAGRAM_MAX || tx.count >= TX_QUEUE_LEN)
        return false;

    tx_entry_t *e = &tx.queue[tx.count++];
    e->dst = dst_alias;
    e->len = len;
    memcpy(e->buf, data, len);

    if (tx.count == 1)
        transmit_head(lcc_now);
    return true;
}

bool lcc_datagram_message(uint16_t mti, uint16_t src, const uint8_t *data,
                          uint8_t len, uint32_t now)
{
    if (tx.count == 0 || !tx.in_flight || tx.queue[0].dst != src)
        return false;

    if (mti == LCC_MTI_DATAGRAM_OK) {
        pop_head(now);
        return true;
    }

    uint16_t code = len >= 2 ? lcc_get_be16(data) : LCC_ERROR_PERMANENT;
    if ((code & LCC_ERROR_TEMPORARY) && tx.tries < TX_MAX_TRIES) {
        tx.in_flight = false;
        tx.resend_wait = true;
        tx.deadline = now + TX_RESEND_DELAY_MS;
    } else {
        pop_head(now);
    }
    return true;
}

void lcc_datagram_alias_lost(void)
{
    /* Replies addressed with the old alias would be ignored; drop them */
    tx.count = 0;
    tx.in_flight = false;
    tx.resend_wait = false;
    tx.tries = 0;
    rx.active = false;
    rx.refusing = false;
}

/* ------------------------------------------------------------------ */
/* Timers and setup                                                   */
/* ------------------------------------------------------------------ */

void lcc_datagram_run_timers(uint32_t now)
{
    if (rx.active && lcc_time_reached(now, rx.deadline)) {
        rx.active = false;
        rx.refusing = false;
    }
    if (tx.resend_wait && lcc_time_reached(now, tx.deadline))
        transmit_head(now);
    else if (tx.in_flight && lcc_time_reached(now, tx.deadline))
        pop_head(now);
}

uint32_t lcc_datagram_next_timer(uint32_t now)
{
    uint32_t next = LCC_NO_TIMER;
    lcc_timer_min(&next, rx.active, now, rx.deadline);
    lcc_timer_min(&next, tx.in_flight || tx.resend_wait, now, tx.deadline);
    return next;
}

void lcc_datagram_reset(void)
{
    handler_count = 0;
    memset(&rx, 0, sizeof(rx));
    memset(&tx, 0, sizeof(tx));
}

bool lcc_datagram_register(const lcc_datagram_handler_t *handler)
{
    if (handler_count >= LCC_MAX_DATAGRAM_HANDLERS)
        return false;
    handlers[handler_count++] = handler;
    return true;
}

bool lcc_datagram_has_handlers(void)
{
    return handler_count > 0;
}
