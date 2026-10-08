#ifndef HERMES_DRV_GPIO_H
#define HERMES_DRV_GPIO_H

/* Core driver: digital pins. The only place in HERMES that knows the
 * board's pin map and touches DDRx/PORTx/PINx -- devices and modules
 * (e.g. modules/gpio) go through this instead of poking registers.
 *
 * Pin numbering = Arduino Uno (ATmega328P):
 *   D0-D7   -> PORTD bits 0-7   (D0/D1 are also UART0 RX/TX)
 *   D8-D13  -> PORTB bits 0-5   (D13 = built-in LED)
 *   A0-A5   -> PORTC bits 0-5, numbered 14-19 as on Arduino
 * Any other number is invalid: every function ignores it (reads LOW).
 *
 * Host: no real pins -- a mock keeps each pin's mode/level in memory and
 * traces every write to stdout (`[gpio mock] pin N -> HIGH/LOW`), and
 * hermes_gpio_mock_set_input lets a host-side test drive what reads see. */

#include <stdbool.h>

#define HERMES_GPIO_PIN_COUNT 20

typedef enum {
    HERMES_GPIO_OUTPUT,
    HERMES_GPIO_INPUT,
    HERMES_GPIO_INPUT_PULLUP,
} hermes_gpio_mode_t;

/* 1 if `pin` exists on this board. */
int hermes_gpio_valid(int pin);

/* Configures `pin`. Returns 0 (and does nothing) if the pin is invalid. */
int hermes_gpio_mode(int pin, hermes_gpio_mode_t mode);

void hermes_gpio_write(int pin, bool high);
bool hermes_gpio_read(int pin);

#if !defined(__AVR__)
/* Host only: level an input pin will read from now on. */
void hermes_gpio_mock_set_input(int pin, bool high);
#endif

#endif
