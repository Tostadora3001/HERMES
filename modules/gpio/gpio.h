#ifndef HERMES_MODULE_GPIO_H
#define HERMES_MODULE_GPIO_H

/* Module `gpio` (modules/gpio/, described by module.hmod next to this
 * file; `import gpio` in a .hepy): one digital pin as a device.
 *
 *   device led = gpio(13)              output
 *   device btn = gpio_in(2, True)      input, with internal pull-up
 *   led.write(True)
 *   bool pressed = btn.read()
 *   led.abort()                        mandatory abort: output -> LOW
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
#include "hermes_sched_internal.h" /* hermes_runtime_alloc: same pool policy as threads/semaphores */
#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    hermes_device_t device;
    int pin;
    unsigned char mode; /* hermes_gpio_mode_t it was created with */
} hermes_device_gpio_t;

typedef hermes_device_gpio_t *hermes_device_gpio_handle_t;

/* Mandatory abort (hermes_device.h): an output goes LOW -- the inactive
 * level for the usual active-high load (LED, transistor/relay driver). An
 * input drives nothing, so there's nothing to deactivate. */
static inline void device_gpio_abort(hermes_device_gpio_handle_t g) {
    if (g->mode == HERMES_GPIO_OUTPUT) hermes_gpio_write(g->pin, false);
}

/* Device class (see hermes_device.h): `static` via the macro is fine here,
 * only the generated .c includes this header (one copy). Per-kind limit =
 * every pin the driver knows (D0-D13 + A0-A5). */
HERMES_DEVICE_CLASS(gpio, hermes_device_gpio_handle_t, HERMES_GPIO_PIN_COUNT);

static inline hermes_device_gpio_handle_t hermes_gpio_device_new(long pin, hermes_gpio_mode_t mode) {
    hermes_device_gpio_t *g = hermes_runtime_alloc(sizeof(hermes_device_gpio_t));
    if (!g) {
        hermes_fatal(); /* same contract as hermes_thread_create: a device you asked for and didn't get is fatal */
    }
    hermes_device_register_or_abort(&g->device, &hermes_device_class_gpio, "gpio");
    g->pin = (int)pin;
    g->mode = (unsigned char)mode;
    hermes_gpio_mode(g->pin, mode); /* invalid pin: ignored, as by the driver */
    return g;
}

static inline hermes_device_gpio_handle_t gpio(long pin) {
    return hermes_gpio_device_new(pin, HERMES_GPIO_OUTPUT);
}

static inline hermes_device_gpio_handle_t gpio_in(long pin, bool pullup) {
    return hermes_gpio_device_new(pin, pullup ? HERMES_GPIO_INPUT_PULLUP : HERMES_GPIO_INPUT);
}

static inline void device_gpio_write(hermes_device_gpio_handle_t g, bool value) {
    hermes_gpio_write(g->pin, value);
}

static inline bool device_gpio_read(hermes_device_gpio_handle_t g) {
    return hermes_gpio_read(g->pin);
}

#endif
