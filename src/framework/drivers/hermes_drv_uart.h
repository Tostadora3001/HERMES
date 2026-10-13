#ifndef HERMES_DRV_UART_H
#define HERMES_DRV_UART_H

/* Core driver: hardware serial port (UART0 / USART0: pins D0=RX / D1=TX
 * on an Uno and on a Mega; GPIO20=RX / GPIO21=TX on an ESP32-C3).
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
 * Host: TX writes to stdout (same stream as print(), so ordering holds);
 * RX reads stdin without blocking. The baud rate is accepted and ignored. */

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

/* Implemented by each target. */

/* (Re)configures the port. Safe to call again to change the baud rate. */
void hermes_uart_init(uint32_t baud);

void hermes_uart_write_byte(uint8_t b);

/* Stops receiving (RX and its interrupt off, pending bytes discarded);
 * TX keeps working. hermes_uart_init turns RX back on. Used by the uart
 * device's mandatory abort. */
void hermes_uart_rx_disable(void);

/* Bytes waiting in the RX buffer (host: 1 if stdin has data, else 0). */
int hermes_uart_available(void);

/* Next received byte, or -1 if none is waiting (never blocks). */
int hermes_uart_read_byte(void);

/* Common part. */
void hermes_uart_write_str(const char *s);

#endif
