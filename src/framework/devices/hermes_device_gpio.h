#ifndef HERMES_DEVICE_GPIO_H
#define HERMES_DEVICE_GPIO_H

/* HERMES's first *external* device: a single digital output pin. Real
 * register pokes on AVR (Arduino Uno-style numbering, D0-D13, same
 * convention as Arduino's digitalWrite -- no driver layer exists yet in
 * src/framework/drivers, so this talks to DDRx/PORTx directly, same as
 * hermes_ctx_avr.S already does for SPL/SPH/SREG). On the host there is no
 * real pin to drive, so a mock just records the last value written and
 * traces it to stdout -- enough to verify program logic (and to run the
 * same .hepy on `native` for testing before flashing `avr`).
 *
 * `gpio(long pin)` is the HERMESPY-visible constructor name -- it must
 * match the `constructor_name` entry in src/engine/compiler/devicekind.c
 * exactly, since that registry is what makes `device led = gpio(13)`
 * resolve to this file's handle type and `led.write(...)` resolve to
 * `device_gpio_write`.
 */

#include "hermes_device.h"
#include "hermes_sched_internal.h" /* hermes_runtime_alloc: same pool policy as threads/semaphores */
#include <stdbool.h>
#include <stdlib.h>

#if defined(__AVR__)
#include <avr/io.h>
#else
#include <stdio.h>
#endif

typedef struct {
    hermes_device_t device;
    int pin;
    bool mock_state; /* host only; harmless to carry on AVR too */
} hermes_device_gpio_t;

typedef hermes_device_gpio_t *hermes_device_gpio_handle_t;

#if defined(__AVR__)

typedef struct {
    volatile uint8_t *ddr;
    volatile uint8_t *port;
    uint8_t bit;
} hermes_gpio_pin_info_t;

/* Arduino Uno / ATmega328P digital pin map: D0-D7 -> PORTD bits 0-7,
 * D8-D13 -> PORTB bits 0-5. Returns 0 (and leaves *out untouched) for any
 * pin outside 0-13 -- there is no analog/extended-pin support yet. */
static inline int hermes_gpio_pin_info(int pin, hermes_gpio_pin_info_t *out) {
    if (pin >= 0 && pin <= 7) {
        out->ddr = &DDRD; out->port = &PORTD; out->bit = (uint8_t)pin;
        return 1;
    }
    if (pin >= 8 && pin <= 13) {
        out->ddr = &DDRB; out->port = &PORTB; out->bit = (uint8_t)(pin - 8);
        return 1;
    }
    return 0;
}

#endif

static inline hermes_device_gpio_handle_t gpio(long pin) {
    hermes_device_gpio_t *g = hermes_runtime_alloc(sizeof(hermes_device_gpio_t));
    if (!g) {
        abort(); /* same contract as hermes_thread_create: a device you asked for and didn't get is fatal */
    }
    g->device.kind = HERMES_DEVICE_GPIO;
    g->device.name = "gpio";
    g->pin = (int)pin;
    g->mock_state = false;

#if defined(__AVR__)
    hermes_gpio_pin_info_t info;
    if (hermes_gpio_pin_info(g->pin, &info)) {
        *info.ddr |= (uint8_t)(1u << info.bit); /* configure as output */
    }
#endif

    return g;
}

static inline void device_gpio_write(hermes_device_gpio_handle_t g, bool value) {
#if defined(__AVR__)
    hermes_gpio_pin_info_t info;
    if (hermes_gpio_pin_info(g->pin, &info)) {
        if (value) *info.port |= (uint8_t)(1u << info.bit);
        else *info.port &= (uint8_t)~(1u << info.bit);
    }
#else
    g->mock_state = value;
    printf("[gpio mock] pin %d -> %s\n", g->pin, value ? "HIGH" : "LOW");
#endif
}

#endif
