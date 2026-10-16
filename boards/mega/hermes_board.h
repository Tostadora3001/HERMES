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

/* Analog inputs (ADC driver): X(pin, channel). A0-A15 = pins 54-69 =
 * channels 0-15. Reference: AVcc, HERMES_BOARD_ADC_VREF_MV. */
#define HERMES_BOARD_ADC_PINS(X)                                        \
    X(54, 0) X(55, 1) X(56, 2) X(57, 3) X(58, 4) X(59, 5) X(60, 6)      \
    X(61, 7) X(62, 8) X(63, 9) X(64, 10) X(65, 11) X(66, 12) X(67, 13)  \
    X(68, 14) X(69, 15)
#define HERMES_BOARD_ADC_VREF_MV 5000

/* PWM outputs (PWM driver): X(pin, timer, channel, bits), as Arduino's
 * Mega. Timer0 (D4, D13) belongs to the system timer driver: listed so the
 * error can say so. */
#define HERMES_BOARD_PWM_PINS(X)                                        \
    X(2, 3, B, 16) X(3, 3, C, 16) X(4, 0, B, 8) X(5, 3, A, 16)          \
    X(6, 4, A, 16) X(7, 4, B, 16) X(8, 4, C, 16) X(9, 2, B, 8)          \
    X(10, 2, A, 8) X(11, 1, A, 16) X(12, 1, B, 16) X(13, 0, A, 8)       \
    X(44, 5, C, 16) X(45, 5, B, 16) X(46, 5, A, 16)

/* The serial port's pins (the uart device holds them: hardware conflicts,
 * hermes_device.h). */
#define HERMES_BOARD_UART_RX_PIN 0
#define HERMES_BOARD_UART_TX_PIN 1
/* Serial ports (UART driver): X(port, rx pin, tx pin); port 0 is print()'s.
 * USART0-3: RX0/TX0 = D0/D1, RX1/TX1 = D19/D18, RX2/TX2 = D17/D16, RX3/TX3 = D15/D14. */
#define HERMES_BOARD_UART_PINS(X) X(0, 0, 1) X(1, 19, 18) X(2, 17, 16) X(3, 15, 14)
/* The SPI bus's pins (SPI driver, master; SCK D52 = PB1, MOSI D51 = PB2, MISO D50 = PB3, SS D53 = PB0). SS must stay an
 * output while master: the driver makes it one. */
#define HERMES_BOARD_SPI_SCK_PIN 52
#define HERMES_BOARD_SPI_MOSI_PIN 51
#define HERMES_BOARD_SPI_MISO_PIN 50
#define HERMES_BOARD_SPI_SS_PIN 53

/* The I2C bus's pins (TWI: SDA = D20 = PD1, SCL = D21 = PD0; the i2c and
 * i2c_slave devices hold them). */
#define HERMES_BOARD_I2C_SDA_PIN 20
#define HERMES_BOARD_I2C_SCL_PIN 21

/* Hardware timers (hermes_timers): X(number, bits), numbered 0..N-1.
 * Timer0 is the system's by default (millis, delay_ms, the OS tick). */
#define HERMES_BOARD_TIMERS(X) X(0, 8) X(1, 16) X(2, 8) X(3, 16) X(4, 16) X(5, 16)

#endif
