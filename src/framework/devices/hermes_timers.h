#ifndef HERMES_TIMERS_H
#define HERMES_TIMERS_H

/* `import hermes_timers`: every hardware timer as a built-in (core)
 * device with a ROLE (system, pwm, reserved, free) and the methods of that
 * role. A role can always be changed to any other: what each timer can do
 * on the chip is the programmer's business (decided with the user).
 * See docs/raw/hermes_timers.en.md and hermes_drv_timers.h.
 *
 *   device t2 = timer(2)        Uno: 0-2; Mega: 0-5 (doesn't change its role)
 *   print(t2.role())            "system", "pwm", "reserved" or "free"
 *   t2.to_system()              millis/delay_ms/the OS tick move to it (the
 *                               old system timer becomes free)
 *   t2.to_pwm()  t2.to_reserved()  t2.to_free()
 *
 *   role pwm:      t.freq(hz) -> Hz set, t.frequency(), t.pins()
 *   role system:   t.millis(), t.micros()
 *   role reserved: t.owner()    "user"
 * A method of another role is a fatal error naming the timer's role.
 *
 * Consequences, by design:
 *   - leaving the pwm role, its pwm() pins go LOW and off; writing them is
 *     an error until the timer is pwm again;
 *   - leaving the system role without giving it to another timer, millis()
 *     stands still and delay_ms() never ends;
 *   - with hermes_os linked, the system role can only change before the
 *     timer is first used (millis, delay_ms, os_run...); without it, at
 *     any time (millis goes on from the same value on the new timer).
 *
 * abort (mandatory): a pwm timer drives its pins to duty 0; the others do
 * nothing (the system timer keeps counting: the program can still
 * report).
 *
 * t.destroy(): only the device goes (memory released, the variable left
 * NULL): the timer keeps its role -- the system one keeps counting, a pwm
 * one keeps driving its pins (decided with the user) -- and timer(n) can
 * be created again. No abort. Any method after destroy() is a fatal
 * error. */

#include "hermes_device.h"
#include "hermes_drv_timers.h"
#include "hermes_drv_timer.h"
#include "hermes_drv_pwm.h"
#include "hermes_sched_internal.h" /* hermes_runtime_alloc, hermes_lock */
#include "hermes_diag.h"

typedef struct {
    hermes_device_t device;
    signed char n;
} hermes_device_timer_t;

typedef hermes_handle_t hermes_device_timer_handle_t;
HERMES_DEVICE_KIND(timer);

/* Weak: defined only when hermes_os is linked. */
extern volatile unsigned char hermes_os_working __attribute__((weak));

static inline void device_timer_abort(hermes_device_timer_handle_t h_t) {
    hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "abort");
    if (hermes_timers_role(t->n) == HERMES_TIMER_PWM) hermes_pwm_timer_zero(t->n);
}

/* Mandatory describe (hermes_device.h): no pins of its own (a pwm
 * timer's pins are its pwm devices'), memory, number and role. */
static inline void device_timer_describe(hermes_device_timer_handle_t h_t, hermes_device_desc_t *o) {
    hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "describe");
    o->bytes = sizeof(hermes_device_timer_t);
    (void)t;
#ifdef HERMES_DEVICES /* the dump's fields */
    HERMES_DEVICE_VALUE(o, "timer", t->n);
    const char *r;
    switch (hermes_timers_role(t->n)) {
    case HERMES_TIMER_SYSTEM: r = HERMES_DEVICE_STR("system"); break;
    case HERMES_TIMER_PWM: r = HERMES_DEVICE_STR("pwm"); break;
    case HERMES_TIMER_RESERVED: r = HERMES_DEVICE_STR("reserved"); break;
    default: r = HERMES_DEVICE_STR("free"); break;
    }
    HERMES_DEVICE_TEXT(o, "role", r);
#endif
}

/* static via the macro: only the generated .c includes this header. One
 * device per hardware timer. */
HERMES_DEVICE_CLASS(timer, hermes_device_timer_handle_t, HERMES_TIMERS_COUNT);

static inline hermes_device_timer_handle_t timer(long n) {
    if (!hermes_timers_exists((int)n)) {
        HERMES_DIAG("hermes_timers: timer(%ld): board '%" HERMES_PS "' has timers 0-%d\n",
                    n, HERMES_PSTR(HERMES_BOARD_NAME), HERMES_TIMERS_COUNT - 1);
        hermes_fatal();
    }
    hermes_lock();
    for (hermes_device_t *d = hermes_device_class_timer.head; d; d = d->next) {
        if (((hermes_device_timer_t *)d)->n != n) continue;
        hermes_unlock();
        HERMES_DIAG("hermes_timers: timer(%ld): that timer already is a timer device\n", n);
        hermes_fatal();
    }
    hermes_device_timer_t *t = hermes_runtime_alloc(sizeof(hermes_device_timer_t));
    if (!t) {
        hermes_unlock();
        HERMES_DIAG("hermes_timers: timer(%ld): out of memory (%u bytes)\n", n, (unsigned)sizeof(hermes_device_timer_t));
        hermes_fatal();
    }
    t->n = (signed char)n;
    hermes_device_register_or_abort(&t->device, &hermes_device_class_timer);
    hermes_unlock();
    return hermes_device_handle(&t->device);
}

/* destroy: the device only; the timer keeps its role (see above). */
static inline void device_timer_destroy(hermes_device_timer_handle_t h_t) {
    hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "destroy");
    hermes_device_destroy(&t->device, 0);
}

static inline const char *device_timer_role(hermes_device_timer_handle_t h_t) {
    hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "role");
    hermes_lock();
    const char *r = hermes_timers_role_name(hermes_timers_role(t->n));
    hermes_unlock();
    return r;
}

/* With the OS, the system timer is fixed once in use. */
static inline void hermes_timer_role_change(hermes_device_timer_t *t, hermes_timer_role_t r) {
    hermes_lock();
    int touches_system = r == HERMES_TIMER_SYSTEM || hermes_timers_role(t->n) == HERMES_TIMER_SYSTEM;
    if (touches_system && &hermes_os_working && hermes_timer_started() && hermes_timers_role(t->n) != r) {
        hermes_unlock();
        HERMES_DIAG("hermes_timers: Timer%d: the system role can't change once the timer is in use "
                    "with hermes_os (change it before millis, delay_ms or os_run)\n", t->n);
        hermes_fatal();
    }
    hermes_timers_set_role(t->n, r, HERMES_TIMER_OWNER_USER);
    hermes_unlock();
}

static inline void device_timer_to_system(hermes_device_timer_handle_t h_t) { hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "to_system"); hermes_timer_role_change(t, HERMES_TIMER_SYSTEM); }
static inline void device_timer_to_pwm(hermes_device_timer_handle_t h_t) { hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "to_pwm"); hermes_timer_role_change(t, HERMES_TIMER_PWM); }
static inline void device_timer_to_reserved(hermes_device_timer_handle_t h_t) { hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "to_reserved"); hermes_timer_role_change(t, HERMES_TIMER_RESERVED); }
static inline void device_timer_to_free(hermes_device_timer_handle_t h_t) { hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "to_free"); hermes_timer_role_change(t, HERMES_TIMER_FREE); }

/* A method of role `need` on timer t: fatal if t has another role.
 * `what` is a HERMES_PSTR text. */
static inline void hermes_timer_need(hermes_device_timer_t *t, hermes_timer_role_t need, const char *what) {
    hermes_timer_role_t r = hermes_timers_role(t->n);
    if (r == need) return;
    HERMES_DIAG("hermes_timers: %" HERMES_PS "() on Timer%d, whose role is '%" HERMES_PS "' (it needs '%" HERMES_PS "')\n",
                what, t->n, hermes_timers_role_pname(r), hermes_timers_role_pname(need));
    (void)what;
    hermes_fatal();
}

/* --- role pwm --- */

static inline long device_timer_freq(hermes_device_timer_handle_t h_t, long hz) {
    hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "freq");
    hermes_timer_need(t, HERMES_TIMER_PWM, HERMES_PSTR("freq"));
    if (hz < 1) {
        HERMES_DIAG("hermes_timers: freq(%ld) on Timer%d: the frequency must be at least 1 Hz\n", hz, t->n);
        hermes_fatal();
    }
    hermes_lock();
    long got = hermes_pwm_timer_freq(t->n, hz);
    hermes_unlock();
    if (!got) {
        HERMES_DIAG("hermes_timers: freq(%ld) on Timer%d: too high for it\n", hz, t->n);
        hermes_fatal();
    }
    return got;
}

static inline long device_timer_frequency(hermes_device_timer_handle_t h_t) {
    hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "frequency");
    hermes_timer_need(t, HERMES_TIMER_PWM, HERMES_PSTR("frequency"));
    hermes_lock();
    long f = hermes_pwm_timer_freq_now(t->n);
    hermes_unlock();
    return f;
}

static inline long device_timer_pins(hermes_device_timer_handle_t h_t) {
    hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "pins");
    hermes_timer_need(t, HERMES_TIMER_PWM, HERMES_PSTR("pins"));
    hermes_lock();
    long n = hermes_pwm_timer_pins(t->n);
    hermes_unlock();
    return n;
}

/* --- role system --- */

static inline long device_timer_millis(hermes_device_timer_handle_t h_t) {
    hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "millis");
    hermes_timer_need(t, HERMES_TIMER_SYSTEM, HERMES_PSTR("millis"));
    return (long)hermes_timer_millis();
}

static inline long device_timer_micros(hermes_device_timer_handle_t h_t) {
    hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "micros");
    hermes_timer_need(t, HERMES_TIMER_SYSTEM, HERMES_PSTR("micros"));
    return (long)hermes_timer_micros();
}

/* --- role reserved --- */

static inline const char *device_timer_owner(hermes_device_timer_handle_t h_t) {
    hermes_device_timer_t *t = HERMES_DEVICE_GET(hermes_device_timer_t, timer, h_t, "owner");
    hermes_timer_need(t, HERMES_TIMER_RESERVED, HERMES_PSTR("owner"));
    return hermes_timers_owner_name(hermes_timers_owner(t->n));
}

#endif
