#ifndef HERMES_DRV_UART_H
#define HERMES_DRV_UART_H

/* Core driver: hardware serial ports. Port 0 is UART0 / USART0 (pins
 * D0=RX / D1=TX on an Uno and on a Mega; GPIO20=RX / GPIO21=TX on an
 * ESP32-C3), the one print() uses. More ports where the chip has them
 * (2026-10-16, asked by the user): the board lists them in
 * HERMES_BOARD_UART_PINS(X), X(port, rx pin, tx pin) -- Uno: 0; Mega:
 * 0-3 (USART1 RX 19 / TX 18, USART2 17 / 16, USART3 15 / 14); ESP32-C3:
 * 0 and 1 (UART1 through the GPIO matrix, GPIO6 RX / GPIO7 TX).
 *
 * Common interface; one implementation per platform:
 *   hermes_drv_uart.c                   common part (write_str)
 *   avr/hermes_drv_uart_avr.c           USART0, RX interrupt + ring buffer
 *   esp32c3/hermes_drv_uart_esp32c3.c   UART0, RX = its 128-byte hardware FIFO
 *   native/hermes_drv_uart_native.c     stdout / stdin
 * hepyc links the common part and the one for the target's platform (the
 * board's chip family: boards/<board>/board.hboard, key `platform`).
 *
 * AVR: 8N1, double-speed mode (U2X0) for a lower baud-rate error at
 * 115200. TX is polled (each byte waits for the data register to be
 * free; ~1 ms per byte at 9600 baud) -- deliberately NOT yielding to other
 * threads while waiting, so a print() line from one thread is never
 * interleaved with another's. RX is interrupt-driven into a ring buffer of
 * HERMES_UART_RX_BUFFER bytes (power of two); bytes arriving while it's
 * full are dropped.
 *
 * print() integration: on bare metal (AVR, ESP32-C3) this driver
 * provides the real `hermes_putchar` (hermes_stdlib.h has a weak no-op
 * one), so as soon as the driver is linked, print() comes out of the
 * serial port. If nothing initialized the UART before the first byte, AVR
 * self-initializes at HERMES_UART_DEFAULT_BAUD; the ESP32-C3 keeps what
 * its ROM configured (115200).
 *
 * Host: port 0's TX writes to stdout (same stream as print(), so ordering
 * holds); its RX reads stdin without blocking. Ports 1-3 are wires in
 * memory (see hermes_uart_mock_faults). The baud rate is accepted and
 * ignored. */

#include <stdint.h>

#ifndef HERMES_UART_DEFAULT_BAUD
#if defined(__riscv)
#define HERMES_UART_DEFAULT_BAUD 115200UL /* ESP32-C3: what its ROM leaves configured */
#else
#define HERMES_UART_DEFAULT_BAUD 9600UL
#endif
#endif

#ifndef HERMES_UART_RX_BUFFER
#define HERMES_UART_RX_BUFFER 32
#endif

/* Implemented by each target, per port (0 .. ports - 1). */

/* Does the port exist on this chip/board? (host: 0-3, see below) */
int hermes_uart_port_exists(int port);

/* (Re)configures the port. Safe to call again to change the baud rate. */
void hermes_uart_port_init(int port, uint32_t baud);

void hermes_uart_port_write_byte(int port, uint8_t b);

/* Stops receiving (RX and its interrupt off, pending bytes discarded);
 * TX keeps working. hermes_uart_port_init turns RX back on. Used by the
 * uart device's mandatory abort. */
void hermes_uart_port_rx_disable(int port);

/* Bytes waiting in the RX buffer (host port 0: 1 if stdin has data). */
int hermes_uart_port_available(int port);

/* Next received byte, or -1 if none is waiting (never blocks). */
int hermes_uart_port_read_byte(int port);

/* Port 0 (print()'s), the names used before ports existed. */
#define hermes_uart_init(baud)       hermes_uart_port_init(0, (baud))
#define hermes_uart_write_byte(b)    hermes_uart_port_write_byte(0, (b))
#define hermes_uart_rx_disable()     hermes_uart_port_rx_disable(0)
#define hermes_uart_available()      hermes_uart_port_available(0)
#define hermes_uart_read_byte()      hermes_uart_port_read_byte(0)

/* Common part. */
void hermes_uart_port_write_str(int port, const char *s);
#define hermes_uart_write_str(s)     hermes_uart_port_write_str(0, (s))

/* Host only: the other ports are wires in memory -- port 1's TX goes into
 * port 2's RX and back (a crossed cable), port 3's TX into its own RX (a
 * loopback): what scripts/simavr_bench does on a Mega with --wire 1:2
 * --wire 3:3. Faults to test a protocol: every Nth byte on those wires
 * lost / with a bit flipped (0 = never), as the bench's --drop/--flip. */
void hermes_uart_mock_faults(int drop_every, int flip_every);

/* Host only: a virtual device on port `port` (1-3) instead of the wire --
 * what a module's mock puts there (the ESP-01's AT firmware, a Bluetooth
 * module...). `from_mcu` gets every byte the program writes; `poll` is
 * asked for the device's next byte (-1: none) whenever the program looks
 * at the port. */
typedef struct {
    void (*from_mcu)(void *ctx, uint8_t b);
    int (*poll)(void *ctx);
    void *ctx;
} hermes_uart_mock_device_t;
void hermes_uart_mock_attach(int port, const hermes_uart_mock_device_t *dev);

#endif
