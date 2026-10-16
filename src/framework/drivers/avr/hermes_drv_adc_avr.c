/* ADC driver, AVR implementation (megaAVR: ATmega328P, ATmega2560...).
 * See hermes_drv_adc.h.
 *
 * AVcc reference, right-adjusted result, prescaler 128 (125 kHz at 16 MHz:
 * within the 50-200 kHz the datasheet asks for 10-bit precision). One
 * conversion at a time, by polling ADSC. Channels 8-15 (ATmega2560) set
 * MUX5 in ADCSRB. The ADC is enabled on the first read. */
#include "hermes_drv_adc.h"
#include <avr/io.h>

/* Weak: NULL without the thread runtime (see the header). */
extern void hermes_preempt_off(void) __attribute__((weak));
extern void hermes_preempt_on(void) __attribute__((weak));

#if F_CPU / 128 > 200000UL
#error "hermes_drv_adc: F_CPU too high for prescaler 128 (ADC clock over 200 kHz)"
#endif

uint16_t hermes_adc_read(int channel) {
    if (hermes_preempt_off) hermes_preempt_off();
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
#ifdef MUX5
    if (channel & 8) ADCSRB |= (1 << MUX5);
    else ADCSRB &= (uint8_t)~(1 << MUX5);
#endif
    ADMUX = (1 << REFS0) | (uint8_t)(channel & 7);
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC)) {
    }
    uint16_t v = ADC; /* ADCL then ADCH: avr-gcc reads the pair in order */
    if (hermes_preempt_on) hermes_preempt_on();
    return v;
}
