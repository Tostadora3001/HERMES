#ifndef HERMES_BOARD_H
#define HERMES_BOARD_H

/* Board: Arduino Mega 2560 (ATmega2560). See docs/raw/hermes_boards.en.md.
 *
 * Pure description, no registers: also included by the host (native)
 * build, whose GPIO mock then has the same pins as the board.
 *
 * HERMES_BOARD_PINS(X) lists every digital pin in HERMESPY numbering
 * order (pin 0 first) as X(port, bit): `gpio(n)` is the n-th entry.
 * Numbering = Arduino's (its pins_arduino.h for the Mega): D0-D53, then
 * A0-A15 numbered 54-69. D0/D1 are the UART's RX/TX (USART0), D13 the
 * built-in LED. */

#define HERMES_BOARD_NAME "mega"

#define HERMES_BOARD_PINS(X)                                            \
    /* D0-D9 */                                                         \
    X(E, 0) X(E, 1) X(E, 4) X(E, 5) X(G, 5) X(E, 3) X(H, 3) X(H, 4)     \
    X(H, 5) X(H, 6)                                                     \
    /* D10-D21 */                                                       \
    X(B, 4) X(B, 5) X(B, 6) X(B, 7) X(J, 1) X(J, 0) X(H, 1) X(H, 0)     \
    X(D, 3) X(D, 2) X(D, 1) X(D, 0)                                     \
    /* D22-D29 */                                                       \
    X(A, 0) X(A, 1) X(A, 2) X(A, 3) X(A, 4) X(A, 5) X(A, 6) X(A, 7)     \
    /* D30-D37 */                                                       \
    X(C, 7) X(C, 6) X(C, 5) X(C, 4) X(C, 3) X(C, 2) X(C, 1) X(C, 0)     \
    /* D38-D41 */                                                       \
    X(D, 7) X(G, 2) X(G, 1) X(G, 0)                                     \
    /* D42-D49 */                                                       \
    X(L, 7) X(L, 6) X(L, 5) X(L, 4) X(L, 3) X(L, 2) X(L, 1) X(L, 0)     \
    /* D50-D53 */                                                       \
    X(B, 3) X(B, 2) X(B, 1) X(B, 0)                                     \
    /* A0-A7 = 54-61 */                                                 \
    X(F, 0) X(F, 1) X(F, 2) X(F, 3) X(F, 4) X(F, 5) X(F, 6) X(F, 7)     \
    /* A8-A15 = 62-69 */                                                \
    X(K, 0) X(K, 1) X(K, 2) X(K, 3) X(K, 4) X(K, 5) X(K, 6) X(K, 7)

#endif
