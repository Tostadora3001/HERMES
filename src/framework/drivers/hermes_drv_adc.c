/* ADC driver, common part (every target). See hermes_drv_adc.h. */
#include "hermes_drv_adc.h"

int hermes_adc_channel(int pin) {
#define HERMES_ADC_CASE_(p, ch) if (pin == (p)) return (ch);
    HERMES_BOARD_ADC_PINS(HERMES_ADC_CASE_)
#undef HERMES_ADC_CASE_
    (void)pin;
    return -1;
}
