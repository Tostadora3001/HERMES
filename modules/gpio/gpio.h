#ifndef HERMES_MODULE_GPIO_H
#define HERMES_MODULE_GPIO_H

/* Module `gpio` (modules/gpio/, described by module.hmod next to this
 * file; `import gpio` in a .hepy): one digital pin as a device.
 *
 *   device led = gpio(13)              output
 *   device btn = gpio_in(2, True)      input, with internal pull-up
 *   led.write(True)
 *   bool pressed = btn.read()
 *   bool steady = btn.debounced()      read without bounces (see below)
 *   led.abort()                        mandatory abort: output -> LOW
 *   led.destroy()                      abort, pin back to input (free for
 *                                      another device), memory released;
 *                                      the variable is left NULL
 *
 * A pin is one device's: gpio(9) while pwm(9), adc(9), another gpio(9) or
 * the uart (D0/D1) exists is a fatal error naming who has it (hardware
 * conflicts, hermes_device.h). Any method after destroy() is a fatal error.
 *
 * Debouncing: under hermes_os, while os_run() runs, the OS samples every
 * input every HERMES_GPIO_DEBOUNCE_MS ms (the kind's device tick, see
 * hermes_device.h); debounced() gives the last level that held for
 * HERMES_GPIO_DEBOUNCE_SAMPLES samples in a row (5 ms x 4 = 20 ms by
 * default). Without the OS running nobody samples: debounced() is read().
 *
 * Built on the core GPIO driver (src/framework/drivers/hermes_drv_gpio.h,
 * `drivers = gpio` in module.hmod): this file knows nothing about
 * registers or the board's pin map. On the host the driver's mock traces
 * every write (`[gpio mock] pin N -> HIGH/LOW`).
 *
 * The constructor names (`gpio`, `gpio_in`) must match the `device = ...
 * ctor=...` lines of module.hmod exactly -- that's what makes
 * `device led = gpio(13)` resolve to this file's handle type and
 * `led.write(...)` to `device_gpio_write`.
 */

#include "hermes_device.h"
#include "hermes_drv_gpio.h"
#include "hermes_sched_internal.h" /* hermes_runtime_alloc: same allocator as threads/semaphores */
#include "hermes_diag.h"
#include <stdbool.h>
#include <stdlib.h>

#ifndef HERMES_GPIO_DEBOUNCE_MS
#define HERMES_GPIO_DEBOUNCE_MS 5
#endif
#ifndef HERMES_GPIO_DEBOUNCE_SAMPLES
#define HERMES_GPIO_DEBOUNCE_SAMPLES 4
#endif

typedef struct {
    hermes_device_t device;
    int pin;
    unsigned char mode;   /* hermes_gpio_mode_t it was created with */
    unsigned char stable; /* debounced level (inputs) */
    unsigned char differ; /* samples in a row that differ from it */
} hermes_device_gpio_t;

typedef hermes_handle_t hermes_device_gpio_handle_t;
HERMES_DEVICE_KIND(gpio);

/* Mandatory abort (hermes_device.h): an output goes LOW -- the inactive
 * level for the usual active-high load (LED, transistor/relay driver). An
 * input drives nothing, so there's nothing to deactivate. */
static inline void device_gpio_abort(hermes_device_gpio_handle_t h_g) {
    hermes_device_gpio_t *g = HERMES_DEVICE_GET(hermes_device_gpio_t, gpio, h_g, "abort");
    if (g->mode == HERMES_GPIO_OUTPUT) hermes_gpio_write(g->pin, false);
}

/* Mandatory describe (hermes_device.h): its pin, memory and mode. */
static inline void device_gpio_describe(hermes_device_gpio_handle_t h_g, hermes_device_desc_t *o) {
    hermes_device_gpio_t *g = HERMES_DEVICE_GET(hermes_device_gpio_t, gpio, h_g, "describe");
    o->pins[0] = (signed char)g->pin;
    o->bytes = sizeof(hermes_device_gpio_t);
    HERMES_DEVICE_TEXT(o, "mode",
                            g->mode == HERMES_GPIO_OUTPUT ? HERMES_DEVICE_STR("out") :
                            g->mode == HERMES_GPIO_INPUT_PULLUP ? HERMES_DEVICE_STR("in-pullup") :
                            HERMES_DEVICE_STR("in"));
}

/* Device tick (OS context, every HERMES_GPIO_DEBOUNCE_MS ms): an input's
 * new level counts once it holds for HERMES_GPIO_DEBOUNCE_SAMPLES samples. */
static inline void device_gpio_tick(hermes_device_gpio_handle_t h_g) {
    hermes_device_gpio_t *g = HERMES_DEVICE_GET(hermes_device_gpio_t, gpio, h_g, "tick");
    if (g->mode == HERMES_GPIO_OUTPUT) return;
    unsigned char level = hermes_gpio_read(g->pin) ? 1 : 0;
    if (level == g->stable) g->differ = 0;
    else if (++g->differ >= HERMES_GPIO_DEBOUNCE_SAMPLES) { g->stable = level; g->differ = 0; }
}

/* Device class (see hermes_device.h): `static` via the macro is fine here,
 * only the generated .c includes this header (one copy). Per-kind limit =
 * every pin of the board (Uno: D0-D13 + A0-A5; Mega: D0-D53 + A0-A15). */
HERMES_DEVICE_CLASS_TICK(gpio, hermes_device_gpio_handle_t, HERMES_GPIO_PIN_COUNT, HERMES_GPIO_DEBOUNCE_MS);

/* `ctor`: "gpio" or "gpio_in" (HERMES_PSTR), for the messages. The pin is
 * checked and the device registered under one lock: two cores can't take
 * the same pin. */
static inline hermes_device_gpio_handle_t hermes_gpio_device_new(long pin, hermes_gpio_mode_t mode, const char *ctor) {
    hermes_lock();
    hermes_device_pin_claim((int)pin, ctor, pin);
    hermes_device_gpio_t *g = hermes_runtime_alloc(sizeof(hermes_device_gpio_t));
    if (!g) {
        /* same contract as hermes_thread_create: a device you asked for and didn't get is fatal */
        hermes_unlock();
        HERMES_DIAG("gpio: %" HERMES_PS "(%ld): out of memory (%u bytes)\n", ctor, pin, (unsigned)sizeof(hermes_device_gpio_t));
        hermes_fatal();
    }
    g->pin = (int)pin;
    g->mode = HERMES_GPIO_OUTPUT; /* the OS's tick leaves it alone until it's ready */
    g->differ = 0;
    hermes_device_register_or_abort(&g->device, &hermes_device_class_gpio);
    hermes_unlock();
    (void)ctor;
    hermes_gpio_mode(g->pin, mode); /* invalid pin: ignored, as by the driver */
    g->stable = hermes_gpio_read(g->pin) ? 1 : 0;
    g->mode = (unsigned char)mode;
    return hermes_device_handle(&g->device);
}

static inline hermes_device_gpio_handle_t gpio(long pin) {
    return hermes_gpio_device_new(pin, HERMES_GPIO_OUTPUT, HERMES_PSTR("gpio"));
}

static inline hermes_device_gpio_handle_t gpio_in(long pin, bool pullup) {
    return hermes_gpio_device_new(pin, pullup ? HERMES_GPIO_INPUT_PULLUP : HERMES_GPIO_INPUT, HERMES_PSTR("gpio_in"));
}

/* destroy: abort (an output goes LOW), then the pin is an input with no
 * pull-up -- it drives nothing and is free for another device. */
static inline void hermes_gpio_release(hermes_device_t *d) {
    device_gpio_abort(hermes_device_handle(d));
    hermes_gpio_mode(((hermes_device_gpio_t *)d)->pin, HERMES_GPIO_INPUT);
}

static inline void device_gpio_destroy(hermes_device_gpio_handle_t h_g) {
    hermes_device_gpio_t *g = HERMES_DEVICE_GET(hermes_device_gpio_t, gpio, h_g, "destroy");
    hermes_device_destroy(&g->device, hermes_gpio_release);
}

static inline void device_gpio_write(hermes_device_gpio_handle_t h_g, bool value) {
    hermes_device_gpio_t *g = HERMES_DEVICE_GET(hermes_device_gpio_t, gpio, h_g, "write");
    hermes_gpio_write(g->pin, value);
}

static inline bool device_gpio_read(hermes_device_gpio_handle_t h_g) {
    hermes_device_gpio_t *g = HERMES_DEVICE_GET(hermes_device_gpio_t, gpio, h_g, "read");
    return hermes_gpio_read(g->pin);
}

static inline bool device_gpio_debounced(hermes_device_gpio_handle_t h_g) {
    hermes_device_gpio_t *g = HERMES_DEVICE_GET(hermes_device_gpio_t, gpio, h_g, "debounced");
    if (!hermes_device_ticks_on) return hermes_gpio_read(g->pin); /* nobody samples */
    return g->stable != 0;
}

#endif
