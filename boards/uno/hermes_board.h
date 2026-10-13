#ifndef HERMES_BOARD_H
#define HERMES_BOARD_H

/* Board: Arduino Uno (ATmega328P). See docs/raw/hermes_boards.en.md.
 *
 * Pure description, no registers: also included by the host (native)
 * build, whose GPIO mock then has the same pins as the board.
 *
 * HERMES_BOARD_PINS(X) lists every digital pin in HERMESPY numbering
 * order (pin 0 first) as X(port, bit): `gpio(n)` is the n-th entry.
 * Numbering = Arduino's:
 *   D0-D7   -> PD0-PD7   (D0/D1 are also the UART's RX/TX)
 *   D8-D13  -> PB0-PB5   (D13 = built-in LED)
 *   A0-A5   -> PC0-PC5, numbered 14-19 */

#define HERMES_BOARD_NAME "uno"

#define HERMES_BOARD_PINS(X) \
    X(D, 0) X(D, 1) X(D, 2) X(D, 3) X(D, 4) X(D, 5) X(D, 6) X(D, 7) \
    X(B, 0) X(B, 1) X(B, 2) X(B, 3) X(B, 4) X(B, 5)                 \
    X(C, 0) X(C, 1) X(C, 2) X(C, 3) X(C, 4) X(C, 5)

#endif
