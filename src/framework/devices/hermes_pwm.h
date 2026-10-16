#ifndef HERMES_PWM_H
#define HERMES_PWM_H

/* `import hermes_pwm`: a PWM output as a built-in (core) device. hepyc
 * links the PWM driver (src/framework/drivers/hermes_drv_pwm.c), which
 * uses the GPIO driver, and the runtime automatically.
 *
 *   device led = pwm(9)            PWM pin (Uno: 3, 9, 10, 11), duty 0
 *   led.write(128)                 duty 0-255 (0 = LOW, 255 = HIGH)
 *   long f = led.freq(1000)        frequency of the pin's TIMER, as near
 *                                  as it can; returns the one set (Hz)
 *   long now = led.frequency()     the frequency now (490 Hz by default)
 *   led.abort()                    mandatory abort: duty 0 (output LOW)
 *   led.destroy()                  abort, pin off its timer and back to
 *                                  input (free for another device), memory
 *                                  released; the variable is left NULL. The
 *                                  timer keeps its role (and frequency).
 *
 * The frequency belongs to the timer: pins on the same timer share it --
 * freq() on one changes the other too (each keeps its duty). Uno: D9/D10
 * (Timer1, 1 Hz - ~2.6 MHz), D3/D11 (Timer2: 31372, 3921, 980, 490, 245,
 * 122 or 30 Hz). See hermes_drv_pwm.h.
 *
 * Fatal errors (with their message): pwm() on a pin without PWM, on a pin
 * whose timer's role isn't pwm or free (Timer0 = Uno D5/D6 is the system's
 * by default; `import hermes_timers` changes roles), or on a pin another
 * device already holds (a pwm, gpio, adc...: hardware conflicts,
 * hermes_device.h); any method after destroy(); write()/freq() once the pin's timer has
 * stopped being pwm; write() out of 0-255; freq() below 1 Hz or too high for the timer (over
 * F_CPU/6, ~2.67 MHz at 16 MHz, on a 16-bit timer). */

#include "hermes_device.h"
#include "hermes_drv_pwm.h"
#include "hermes_drv_gpio.h"
#include "hermes_drv_timers.h"
#include "hermes_sched_internal.h" /* hermes_runtime_alloc, hermes_lock */
#include "hermes_diag.h"

typedef struct {
    hermes_device_t device;
    signed char pin;
} hermes_device_pwm_t;

typedef hermes_handle_t hermes_device_pwm_handle_t;
HERMES_DEVICE_KIND(pwm);

/* A pin whose timer's role isn't pwm (Timer0 is the system's by default;
 * import hermes_timers changes roles). `what`: a HERMES_PSTR text. */
static inline void hermes_pwm_role_error(long pin, const char *what) {
    int timer = hermes_pwm_timer_of((int)pin);
    HERMES_DIAG("hermes_pwm: %" HERMES_PS "() on pin %ld: its timer, Timer%d, has role '%" HERMES_PS "' "
                "(it needs 'pwm')\n", what, pin, timer, hermes_timers_role_pname(hermes_timers_role(timer)));
    (void)what; (void)timer;
    hermes_fatal();
}

/* Mandatory abort (hermes_device.h): duty 0, the output stays LOW (it can
 * be written again afterwards, like a gpio). */
static inline void device_pwm_abort(hermes_device_pwm_handle_t h_p) {
    hermes_device_pwm_t *p = HERMES_DEVICE_GET(hermes_device_pwm_t, pwm, h_p, "abort");
    hermes_lock();
    (void)hermes_pwm_write(p->pin, 0); /* its timer no longer pwm: already LOW */
    hermes_unlock();
}

/* Mandatory describe (hermes_device.h): its pin, memory, duty and the
 * frequency of its timer (0 while the timer isn't pwm). Lock held. */
static inline void device_pwm_describe(hermes_device_pwm_handle_t h_p, hermes_device_desc_t *o) {
    hermes_device_pwm_t *p = HERMES_DEVICE_GET(hermes_device_pwm_t, pwm, h_p, "describe");
    o->pins[0] = p->pin;
    o->bytes = sizeof(hermes_device_pwm_t);
#ifdef HERMES_DEVICES /* the dump's fields */
    HERMES_DEVICE_VALUE(o, "duty", hermes_pwm_duty_now(p->pin));
    int is_pwm = hermes_timers_role(hermes_pwm_timer_of(p->pin)) == HERMES_TIMER_PWM;
    HERMES_DEVICE_VALUE(o, "freq", is_pwm ? hermes_pwm_freq_now(p->pin) : 0);
#endif
}

/* static via the macro: only the generated .c includes this header. */
HERMES_DEVICE_CLASS(pwm, hermes_device_pwm_handle_t, HERMES_PWM_PIN_COUNT);

static inline hermes_device_pwm_handle_t pwm(long pin) {
    hermes_lock();
    if (hermes_pwm_index((int)pin) < 0) {
        hermes_unlock();
        HERMES_DIAG("hermes_pwm: pwm(%ld): no PWM on that pin on board '%" HERMES_PS "'\n", pin, HERMES_PSTR(HERMES_BOARD_NAME));
        hermes_fatal();
    }
    hermes_device_pin_claim((int)pin, HERMES_PSTR("pwm"), pin);
    hermes_device_pwm_t *p = hermes_runtime_alloc(sizeof(hermes_device_pwm_t));
    if (!p) {
        hermes_unlock();
        HERMES_DIAG("hermes_pwm: pwm(%ld): out of memory (%u bytes)\n", pin, (unsigned)sizeof(hermes_device_pwm_t));
        hermes_fatal();
    }
    hermes_pwm_status_t st = hermes_pwm_start((int)pin);
    if (st != HERMES_PWM_OK) {
        hermes_unlock();
        hermes_runtime_free(p);
        hermes_pwm_role_error(pin, HERMES_PSTR("pwm")); /* (no PWM at all: checked above) */
    }
    p->pin = (signed char)pin;
    hermes_device_register_or_abort(&p->device, &hermes_device_class_pwm);
    hermes_unlock();
    return hermes_device_handle(&p->device);
}

/* destroy: abort (duty 0), the pin off its timer and an input -- it drives
 * nothing and is free for another device. The timer keeps its role. */
static inline void hermes_pwm_release(hermes_device_t *d) {
    hermes_device_pwm_t *p = (hermes_device_pwm_t *)d;
    device_pwm_abort(hermes_device_handle(d));
    hermes_pwm_stop(p->pin);
    hermes_gpio_mode(p->pin, HERMES_GPIO_INPUT);
}

static inline void device_pwm_destroy(hermes_device_pwm_handle_t h_p) {
    hermes_device_pwm_t *p = HERMES_DEVICE_GET(hermes_device_pwm_t, pwm, h_p, "destroy");
    hermes_device_destroy(&p->device, hermes_pwm_release);
}

static inline void device_pwm_write(hermes_device_pwm_handle_t h_p, long duty) {
    hermes_device_pwm_t *p = HERMES_DEVICE_GET(hermes_device_pwm_t, pwm, h_p, "write");
    if (duty < 0 || duty > 255) {
        HERMES_DIAG("hermes_pwm: write(%ld) on pin %d: the duty must be between 0 and 255\n", duty, p->pin);
        hermes_fatal();
    }
    hermes_lock();
    hermes_pwm_status_t st = hermes_pwm_write(p->pin, (uint8_t)duty);
    hermes_unlock();
    if (st != HERMES_PWM_OK) hermes_pwm_role_error(p->pin, HERMES_PSTR("write"));
}

static inline long device_pwm_freq(hermes_device_pwm_handle_t h_p, long hz) {
    hermes_device_pwm_t *p = HERMES_DEVICE_GET(hermes_device_pwm_t, pwm, h_p, "freq");
    if (hz < 1) {
        HERMES_DIAG("hermes_pwm: freq(%ld) on pin %d: the frequency must be at least 1 Hz\n", hz, p->pin);
        hermes_fatal();
    }
    hermes_lock();
    int is_pwm = hermes_timers_role(hermes_pwm_timer_of(p->pin)) == HERMES_TIMER_PWM;
    long got = is_pwm ? hermes_pwm_freq(p->pin, hz) : 0;
    hermes_unlock();
    if (!is_pwm) hermes_pwm_role_error(p->pin, HERMES_PSTR("freq"));
    if (!got) {
        HERMES_DIAG("hermes_pwm: freq(%ld) on pin %d: too high for Timer%d\n",
                    hz, p->pin, hermes_pwm_timer_of(p->pin));
        hermes_fatal();
    }
    return got;
}

static inline long device_pwm_frequency(hermes_device_pwm_handle_t h_p) {
    hermes_device_pwm_t *p = HERMES_DEVICE_GET(hermes_device_pwm_t, pwm, h_p, "frequency");
    hermes_lock();
    long f = hermes_pwm_freq_now(p->pin);
    hermes_unlock();
    return f;
}

#endif
