#ifndef LCC_DATAGRAM_H
#define LCC_DATAGRAM_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Datagram transport: reassembles incoming datagrams, acknowledges or
 * rejects them, and sends outgoing ones with resend on temporary rejection.
 */

#define LCC_DATAGRAM_MAX  72

#ifndef LCC_MAX_DATAGRAM_HANDLERS
#define LCC_MAX_DATAGRAM_HANDLERS  4
#endif

/* Handles datagrams whose first byte is content_type. */
typedef struct {
    uint8_t content_type;
    /* Return 0 to accept or an LCC_ERROR_* code to reject. Set
     * *reply_pending if process() will send a reply datagram. */
    uint16_t (*accept)(uint16_t src_alias, const uint8_t *data, uint8_t len,
                       bool *reply_pending);
    /* Called after Datagram Received OK has gone out. */
    void (*process)(uint16_t src_alias, const uint8_t *data, uint8_t len);
} lcc_datagram_handler_t;

/* The struct must stay valid; returns false when full. */
bool lcc_datagram_register(const lcc_datagram_handler_t *handler);

/* Queue a datagram. Returns false if the queue is full or len is invalid. */
bool lcc_datagram_send(uint16_t dst_alias, const uint8_t *data, uint8_t len);

#endif /* LCC_DATAGRAM_H */
