#include "servo.h"
#include "board.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

static const uint8_t servo_pins[SERVO_COUNT] = {SERVO1_PIN, SERVO2_PIN, SERVO3_PIN, SERVO4_PIN};

void servo_init(void) {
    for (int i = 0; i < SERVO_COUNT; i++) {
        gpio_set_function(servo_pins[i], GPIO_FUNC_PWM);
        uint slice_num = pwm_gpio_to_slice_num(servo_pins[i]);
        
        // Target 50Hz (20ms)
        // clk_sys / (div * wrap) = 50
        // If div = 125, clk_sys / 125 = 1MHz
        // Wrap = 20000
        float div = (float)clock_get_hz(clk_sys) / 1000000.0f;
        pwm_set_clkdiv(slice_num, div);
        pwm_set_wrap(slice_num, 20000);
        
        // Default to center (1.5ms = 1500)
        pwm_set_gpio_level(servo_pins[i], 1500);
        pwm_set_enabled(slice_num, true);
    }
}

void servo_set_position(int index, uint16_t value) {
    if (index < 0 || index >= SERVO_COUNT) return;
    
    // Map 0-65535 to 1000-2000 (1ms to 2ms)
    uint32_t pulse = 1000 + ((uint32_t)value * 1000 / 65535);
    pwm_set_gpio_level(servo_pins[index], pulse);
}
