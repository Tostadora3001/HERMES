/* ADC driver, host (native) implementation: a mock with the board's
 * analog pins. See hermes_drv_adc.h. */
#include "hermes_drv_adc.h"

/* indexed by channel (at most 16 on a megaAVR) */
static volatile uint16_t g_value[HERMES_ADC_PIN_COUNT > 0 ? 16 : 1];

uint16_t hermes_adc_read(int channel) {
    if (channel < 0 || channel >= (int)(sizeof(g_value) / sizeof(g_value[0]))) return 0;
    return g_value[channel];
}

void hermes_adc_mock_set(int pin, int value) {
    int ch = hermes_adc_channel(pin);
    if (ch < 0 || ch >= (int)(sizeof(g_value) / sizeof(g_value[0]))) return;
    if (value < 0) value = 0;
    if (value > HERMES_ADC_MAX) value = HERMES_ADC_MAX;
    g_value[ch] = (uint16_t)value;
}
