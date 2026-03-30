#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "board.h"
#include "lcc_interface.h"

int main()
{
    stdio_init_all();

    gpio_init(HEARTBEAT_LED);
    gpio_set_dir(HEARTBEAT_LED, GPIO_OUT);

    printf("Starting canbus-outpost firmware...\n");

    lcc_interface_init();

    absolute_time_t next_heartbeat = get_absolute_time();
    absolute_time_t next_lcc_tick = get_absolute_time();

    while (true) {
        lcc_interface_run();

        absolute_time_t now = get_absolute_time();

        if (absolute_time_diff_us(now, next_lcc_tick) <= 0) {
            lcc_interface_100ms_tick();
            next_lcc_tick = delayed_by_ms(now, 100);
        }

        if (absolute_time_diff_us(now, next_heartbeat) <= 0) {
            static bool led_state = false;
            led_state = !led_state;
            gpio_put(HEARTBEAT_LED, led_state);
            // 10 second cycle: 100ms on, 9900ms off
            next_heartbeat = delayed_by_ms(now, led_state ? 100 : 9900);
        }
    }
}
