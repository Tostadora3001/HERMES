/* PWM driver, host (native) implementation: a mock that traces every duty
 * written (`[pwm mock] pin N -> duty`). Timers and frequencies are the
 * common part's arithmetic. See hermes_drv_pwm.h. */
#include "hermes_drv_pwm.h"
#include <stdio.h>

void hermes_pwm_hw_timer(int timer, int bits, uint8_t cs, uint16_t top) {
    (void)timer; (void)bits; (void)cs; (void)top;
}

void hermes_pwm_hw_connect(int pin, int timer, int ch, int on) {
    (void)pin; (void)timer; (void)ch; (void)on;
}

void hermes_pwm_hw_compare(int pin, int timer, int ch, uint16_t ocr, uint8_t duty) {
    (void)timer; (void)ch; (void)ocr;
    printf("[pwm mock] pin %d -> %u\n", pin, (unsigned)duty);
}
