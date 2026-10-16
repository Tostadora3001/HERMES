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

/* Analog inputs (ADC driver): X(pin, channel). A0-A5 = pins 14-19 =
 * channels 0-5. Reference: AVcc, HERMES_BOARD_ADC_VREF_MV. */
#define HERMES_BOARD_ADC_PINS(X) \
    X(14, 0) X(15, 1) X(16, 2) X(17, 3) X(18, 4) X(19, 5)
#define HERMES_BOARD_ADC_VREF_MV 5000

/* PWM outputs (PWM driver): X(pin, timer, channel, bits) -- the timer
 * output compare that drives the pin (Arduino's ~ pins). Timer0 (D5, D6)
 * belongs to the system timer driver: listed so the error can say so. */
#define HERMES_BOARD_PWM_PINS(X) \
    X(3, 2, B, 8) X(5, 0, B, 8) X(6, 0, A, 8) X(9, 1, A, 16) X(10, 1, B, 16) X(11, 2, A, 8)

/* The serial port's pins (the uart device holds them: hardware conflicts,
 * hermes_device.h). */
#define HERMES_BOARD_UART_RX_PIN 0
#define HERMES_BOARD_UART_TX_PIN 1
/* Serial ports (UART driver): X(port, rx pin, tx pin); port 0 is print()'s.
 * one USART. */
#define HERMES_BOARD_UART_PINS(X) X(0, 0, 1)
/* The SPI bus's pins (SPI driver, master; SCK D13 = PB5, MOSI D11 = PB3, MISO D12 = PB4, SS D10 = PB2). SS must stay an
 * output while master: the driver makes it one. */
#define HERMES_BOARD_SPI_SCK_PIN 13
#define HERMES_BOARD_SPI_MOSI_PIN 11
#define HERMES_BOARD_SPI_MISO_PIN 12
#define HERMES_BOARD_SPI_SS_PIN 10

/* The I2C bus's pins (TWI: SDA = A4 = PC4, SCL = A5 = PC5; the i2c and
 * i2c_slave devices hold them). */
#define HERMES_BOARD_I2C_SDA_PIN 18
#define HERMES_BOARD_I2C_SCL_PIN 19

/* Hardware timers (hermes_timers): X(number, bits), numbered 0..N-1.
 * Timer0 is the system's by default (millis, delay_ms, the OS tick). */
#define HERMES_BOARD_TIMERS(X) X(0, 8) X(1, 16) X(2, 8)

#endif
