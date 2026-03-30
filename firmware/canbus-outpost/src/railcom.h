#ifndef RAILCOM_H
#define RAILCOM_H

#include <stdint.h>
#include <stdbool.h>

#define RAILCOM_MAX_CHANNELS 4

typedef struct {
    uint16_t address;
    bool occupied;
    // Add more fields if needed (e.g. CV values from Ch 2)
} railcom_data_t;

void railcom_init(void);
void railcom_run(void);

// Callback to be implemented by LCC interface to publish events
extern void railcom_on_data(int channel, railcom_data_t *data);
extern void railcom_on_signal(int channel, bool active);

#endif // RAILCOM_H
