#ifndef HERMES_DRV_ADC_H
#define HERMES_DRV_ADC_H

/* Core driver: analog inputs (ADC). The only place in HERMES that touches
 * the ADC's registers -- the `adc` device (src/framework/devices/
 * hermes_adc.h, `import hermes_adc`) goes through this.
 *
 * Common interface; one implementation per platform:
 *   hermes_drv_adc.c                   common part (which pins are analog)
 *   avr/hermes_drv_adc_avr.c           megaAVR ADC (10 bits, AVcc reference)
 *   native/hermes_drv_adc_native.c     host mock
 * (No ESP32-C3 implementation yet: hepyc says so.)
 *
 * Pins = the board's numbering (boards/<board>/hermes_board.h): the board
 * lists which are analog and their channel (HERMES_BOARD_ADC_PINS) and the
 * reference voltage (HERMES_BOARD_ADC_VREF_MV). Uno: A0-A5 = 14-19;
 * Mega: A0-A15 = 54-69.
 *
 * A conversion takes ~104 us on AVR (125 kHz ADC clock at 16 MHz, 13
 * cycles) and waits for it without yielding. The ADC is one for every
 * pin: under hermes_os on bare metal, a conversion is never interrupted by
 * another thread's (hermes_preempt_off/on around it, weak references --
 * the driver also works without the thread runtime).
 *
 * Host: a mock with the board's analog pins; each reads the value set
 * with hermes_adc_mock_set (0 until then -- what simavr reads too). */

#include <stdint.h>
#include "hermes_platform.h"
#include "hermes_board.h"

#ifndef HERMES_BOARD_ADC_PINS
#define HERMES_BOARD_ADC_PINS(X) /* this board has no analog inputs */
#endif
#ifndef HERMES_BOARD_ADC_VREF_MV
#define HERMES_BOARD_ADC_VREF_MV 5000
#endif

#define HERMES_ADC_MAX 1023 /* 10 bits */

#define HERMES_ADC_COUNT_ONE_(pin, ch) +1
enum { HERMES_ADC_PIN_COUNT = 0 HERMES_BOARD_ADC_PINS(HERMES_ADC_COUNT_ONE_) };

/* Common part: the ADC channel of `pin`, -1 if it isn't analog. */
int hermes_adc_channel(int pin);

/* Implemented by each target: one conversion of `channel` (from
 * hermes_adc_channel), 0..HERMES_ADC_MAX. */
uint16_t hermes_adc_read(int channel);

#if !HERMES_BARE_METAL
/* Host only: what analog `pin` reads from now on (clamped to 0..1023). */
void hermes_adc_mock_set(int pin, int value);
#endif

#endif
