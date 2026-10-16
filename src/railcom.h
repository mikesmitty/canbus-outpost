#ifndef RAILCOM_H
#define RAILCOM_H

#include <stdint.h>
#include <stdbool.h>

#define RAILCOM_MAX_CHANNELS 4

typedef enum {
    RAILCOM_ADDR_SHORT,     // 7-bit base address (CV1)
    RAILCOM_ADDR_LONG,      // 14-bit extended address (CV17/CV18)
    RAILCOM_ADDR_CONSIST,   // 7-bit consist address (CV19)
} railcom_addr_type_t;

// Which way the decoder drove its transmit current through the block's
// TRACK_Rn rail. S-9.3.2 recommends that decoders inject current into the
// rail on their right when facing forward, so for a decoder that follows
// it, A means the TRACK_Rn rail is on the locomotive's right.
typedef enum {
    RAILCOM_ORIENT_UNKNOWN,
    RAILCOM_ORIENT_A,       // current into the TRACK_Rn rail (RC_CHn_A pin)
    RAILCOM_ORIENT_B,       // current out of the TRACK_Rn rail (RC_CHn_B pin)
} railcom_orient_t;

typedef struct {
    uint16_t address;
    bool occupied;
    railcom_addr_type_t addr_type;
    railcom_orient_t orientation;
} railcom_data_t;

// Sets up the PIO sampler and DMA ring. Call before the scheduler starts.
void railcom_init(void);

// FreeRTOS task: decodes each cutout when CUTOUT_SIGNAL falls.
void railcom_task(void *param);

// Callbacks to be implemented by the LCC interface; called from railcom_task.
// railcom_on_data fires when a block's loco changes, and with occupied = false
// (and the last address) when it leaves.
extern void railcom_on_data(int channel, railcom_data_t *data);
extern void railcom_on_signal(int channel, bool active);

#endif // RAILCOM_H
