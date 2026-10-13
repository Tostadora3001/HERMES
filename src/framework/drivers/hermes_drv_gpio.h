#ifndef HERMES_DRV_GPIO_H
#define HERMES_DRV_GPIO_H

/* Core driver: digital pins. The only place in HERMES that touches the
 * pins' registers -- devices and modules (e.g. modules/gpio) go through
 * this instead of poking registers.
 *
 * Common interface; one implementation per platform:
 *   hermes_drv_gpio.c                   common part (pin validity)
 *   avr/hermes_drv_gpio_avr.c           megaAVR registers (DDRx/PORTx/PINx)
 *   esp32c3/hermes_drv_gpio_esp32c3.c   ESP32-C3 GPIO matrix + IO_MUX
 *   native/hermes_drv_gpio_native.c     host mock
 * hepyc links the common part and the one for the target's platform (the
 * board's chip family: boards/<board>/board.hboard, key `platform`).
 *
 * Pin numbering = the board's (boards/<board>/hermes_board.h,
 * HERMES_BOARD_PINS): pin n is its n-th entry. On an Uno, D0-D13 and
 * A0-A5 = 14-19; on a Mega, D0-D53 and A0-A15 = 54-69. Any other number is
 * invalid: every function ignores it (reads LOW).
 *
 * Host: no real pins -- a mock with the pins of the board being built for
 * (the first board imported; without one, the Uno) keeps each pin's
 * mode/level in memory and traces every write to stdout
 * (`[gpio mock] pin N -> HIGH/LOW`), and hermes_gpio_mock_set_input lets a
 * host-side test drive what reads see. */

#include <stdbool.h>
#include "hermes_platform.h"
#include "hermes_board.h"

/* Pins the board lists but a program must not use (e.g. the ESP32-C3's
 * GPIO11-17, wired to the flash chip): the board header may define
 * HERMES_BOARD_PIN_RESERVED(n); they are invalid like a missing pin. */
#ifndef HERMES_BOARD_PIN_RESERVED
#define HERMES_BOARD_PIN_RESERVED(n) 0
#endif

/* Number of pins of the board (an enum: usable in array sizes and static
 * initializers, not in #if). */
#define HERMES_GPIO_COUNT_ONE_(port, bit) +1
enum { HERMES_GPIO_PIN_COUNT = 0 HERMES_BOARD_PINS(HERMES_GPIO_COUNT_ONE_) };

typedef enum {
    HERMES_GPIO_OUTPUT,
    HERMES_GPIO_INPUT,
    HERMES_GPIO_INPUT_PULLUP,
} hermes_gpio_mode_t;

/* 1 if `pin` exists on this board. (Common part.) */
int hermes_gpio_valid(int pin);

/* Implemented by each target. */

/* Configures `pin`. Returns 0 (and does nothing) if the pin is invalid. */
int hermes_gpio_mode(int pin, hermes_gpio_mode_t mode);

void hermes_gpio_write(int pin, bool high);
bool hermes_gpio_read(int pin);

#if !HERMES_BARE_METAL
/* Host only: level an input pin will read from now on. */
void hermes_gpio_mock_set_input(int pin, bool high);
#endif

#endif
