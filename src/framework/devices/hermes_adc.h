#ifndef HERMES_ADC_H
#define HERMES_ADC_H

/* `import hermes_adc`: an analog input as a built-in (core) device.
 * hepyc links the ADC driver (src/framework/drivers/hermes_drv_adc.c) and
 * the runtime automatically.
 *
 *   device pot = adc(14)          analog pin (Uno: A0-A5 = 14-19)
 *   long raw = pot.read()         one conversion, 0-1023
 *   long mv = pot.read_mv()       the same in millivolts (board's reference)
 *   long avg = pot.read_avg(8)    mean of n conversions (1-64), 0-1023
 *   pot.abort()                   mandatory abort: an input drives nothing
 *   pot.destroy()                 memory released, the pin free for another
 *                                 device; the variable is left NULL
 *
 * Fatal errors (with their message): adc() on a pin that isn't analog, or
 * on a pin another device already holds (an adc, gpio, pwm...: hardware
 * conflicts, hermes_device.h); read_avg(n) with n out of 1-64; any method
 * after destroy().
 * One conversion takes ~104 us on AVR and doesn't yield. */

#include "hermes_device.h"
#include "hermes_drv_adc.h"
#include "hermes_sched_internal.h" /* hermes_runtime_alloc, hermes_lock */
#include "hermes_diag.h"

typedef struct {
    hermes_device_t device;
    signed char pin;
    signed char channel;
} hermes_device_adc_t;

typedef hermes_handle_t hermes_device_adc_handle_t;
HERMES_DEVICE_KIND(adc);

/* Mandatory abort (hermes_device.h): an input drives nothing. */
static inline void device_adc_abort(hermes_device_adc_handle_t h_a) {
    (void)HERMES_DEVICE_GET(hermes_device_adc_t, adc, h_a, "abort");
}

/* Mandatory describe (hermes_device.h): its pin, memory and channel. */
static inline void device_adc_describe(hermes_device_adc_handle_t h_a, hermes_device_desc_t *o) {
    hermes_device_adc_t *a = HERMES_DEVICE_GET(hermes_device_adc_t, adc, h_a, "describe");
    o->pins[0] = a->pin;
    o->bytes = sizeof(hermes_device_adc_t);
    HERMES_DEVICE_VALUE(o, "channel", a->channel); (void)a;
}

/* static via the macro: only the generated .c includes this header. One
 * device per analog pin at most. */
HERMES_DEVICE_CLASS(adc, hermes_device_adc_handle_t, HERMES_ADC_PIN_COUNT);

static inline hermes_device_adc_handle_t adc(long pin) {
    int ch = hermes_adc_channel((int)pin);
    if (ch < 0) {
        HERMES_DIAG("hermes_adc: adc(%ld): not an analog pin on board '%" HERMES_PS "'\n", pin, HERMES_PSTR(HERMES_BOARD_NAME));
        hermes_fatal();
    }
    hermes_lock();
    hermes_device_pin_claim((int)pin, HERMES_PSTR("adc"), pin);
    hermes_device_adc_t *a = hermes_runtime_alloc(sizeof(hermes_device_adc_t));
    if (!a) {
        hermes_unlock();
        HERMES_DIAG("hermes_adc: adc(%ld): out of memory (%u bytes)\n", pin, (unsigned)sizeof(hermes_device_adc_t));
        hermes_fatal();
    }
    a->pin = (signed char)pin;
    a->channel = (signed char)ch;
    hermes_device_register_or_abort(&a->device, &hermes_device_class_adc);
    hermes_unlock();
    return hermes_device_handle(&a->device);
}

/* destroy: an analog input drives nothing and stays an input: only the
 * memory and the pin (free for another device). */
static inline void device_adc_destroy(hermes_device_adc_handle_t h_a) {
    hermes_device_adc_t *a = HERMES_DEVICE_GET(hermes_device_adc_t, adc, h_a, "destroy");
    hermes_device_destroy(&a->device, 0);
}

static inline long device_adc_read(hermes_device_adc_handle_t h_a) {
    hermes_device_adc_t *a = HERMES_DEVICE_GET(hermes_device_adc_t, adc, h_a, "read");
    return (long)hermes_adc_read(a->channel);
}

static inline long device_adc_read_mv(hermes_device_adc_handle_t h_a) {
    hermes_device_adc_t *a = HERMES_DEVICE_GET(hermes_device_adc_t, adc, h_a, "read_mv");
    return (long)(((unsigned long)hermes_adc_read(a->channel) * HERMES_BOARD_ADC_VREF_MV
                   + HERMES_ADC_MAX / 2) / HERMES_ADC_MAX);
}

static inline long device_adc_read_avg(hermes_device_adc_handle_t h_a, long n) {
    hermes_device_adc_t *a = HERMES_DEVICE_GET(hermes_device_adc_t, adc, h_a, "read_avg");
    if (n < 1 || n > 64) {
        HERMES_DIAG("hermes_adc: read_avg(%ld): n must be between 1 and 64\n", n);
        hermes_fatal();
    }
    unsigned long sum = 0; /* up to 64 x 1023: more than 16 bits */
    for (long i = 0; i < n; i++) sum += hermes_adc_read(a->channel);
    return (long)((sum + (unsigned long)n / 2) / (unsigned long)n);
}

#endif
