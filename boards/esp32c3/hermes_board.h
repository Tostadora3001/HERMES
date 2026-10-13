#ifndef HERMES_BOARD_H
#define HERMES_BOARD_H

/* Board: ESP32-C3-DevKitM-1 and compatible (ESP32-C3). See
 * docs/raw/hermes_boards.en.md.
 *
 * Pure description, no registers: also included by the host (native)
 * build, whose GPIO mock then has the same pins as the board.
 *
 * HERMESPY pin n = GPIOn (Espressif's numbering): X(GPIO, n) for n =
 * 0..21. GPIO11-17 are reserved (the flash chip: SPI pins, and GPIO11 is
 * VDD_SPI by default) -- invalid like a missing pin. GPIO20/21 are UART0
 * RX/TX (the USB-serial bridge), GPIO18/19 USB D-/D+, GPIO2/8/9 boot
 * strapping pins, GPIO8 the RGB LED of the DevKitM-1. */

#define HERMES_BOARD_NAME "esp32c3"

#define HERMES_BOARD_PINS(X)                                                 \
    X(GPIO, 0)  X(GPIO, 1)  X(GPIO, 2)  X(GPIO, 3)  X(GPIO, 4)  X(GPIO, 5)   \
    X(GPIO, 6)  X(GPIO, 7)  X(GPIO, 8)  X(GPIO, 9)  X(GPIO, 10) X(GPIO, 11)  \
    X(GPIO, 12) X(GPIO, 13) X(GPIO, 14) X(GPIO, 15) X(GPIO, 16) X(GPIO, 17)  \
    X(GPIO, 18) X(GPIO, 19) X(GPIO, 20) X(GPIO, 21)

#define HERMES_BOARD_PIN_RESERVED(n) ((n) >= 11 && (n) <= 17)

#endif
