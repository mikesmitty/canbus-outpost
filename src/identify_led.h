#ifndef IDENTIFY_LED_H
#define IDENTIFY_LED_H

#include "lcc.h"

void identify_led_init(const lcc_identify_config_t *config);
void identify_led_update_config(const lcc_identify_config_t *config);
void identify_led_trigger(const lcc_identify_config_t *config);
bool identify_led_poll(void);

#endif /* IDENTIFY_LED_H */
