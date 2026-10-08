#if !defined(__AVR__)
#define _POSIX_C_SOURCE 200809L /* poll, read */
#endif
#include "hermes_drv_uart.h"

#if (HERMES_UART_RX_BUFFER & (HERMES_UART_RX_BUFFER - 1)) != 0 || HERMES_UART_RX_BUFFER > 256
#error "HERMES_UART_RX_BUFFER debe ser potencia de dos y <= 256"
#endif

void hermes_uart_write_str(const char *s) {
    while (*s) hermes_uart_write_byte((uint8_t)*s++);
}

#if defined(__AVR__)

#include <avr/interrupt.h>
#include <avr/io.h>

#ifndef F_CPU
#error "hermes_drv_uart: F_CPU no definido (hepyc lo fija en el perfil avr)"
#endif

static uint8_t g_initialized = 0;
static volatile uint8_t g_rx_buf[HERMES_UART_RX_BUFFER];
static volatile uint8_t g_rx_head = 0; /* written by the ISR only */
static volatile uint8_t g_rx_tail = 0; /* written by readers only */

#define RX_MASK ((uint8_t)(HERMES_UART_RX_BUFFER - 1))

ISR(USART_RX_vect) {
    uint8_t b = UDR0; /* always read: clears the interrupt */
    uint8_t next = (uint8_t)((g_rx_head + 1) & RX_MASK);
    if (next != g_rx_tail) {
        g_rx_buf[g_rx_head] = b;
        g_rx_head = next;
    } /* else: buffer full, byte dropped */
}

void hermes_uart_init(uint32_t baud) {
    /* double speed: UBRR = F_CPU / (8 * baud) - 1, rounded to nearest */
    uint16_t ubrr = (uint16_t)((F_CPU + 4UL * baud) / (8UL * baud) - 1UL);
    if (g_initialized) {
        while (!(UCSR0A & (1 << UDRE0))) {} /* let the last byte leave */
    }
    /* No `UCSR0B = 0` first to "reset" the port: harmless on real
     * hardware, but under simavr it leaves UDRE0 permanently clear, so the
     * first write_byte waits forever (found while verifying this driver).
     * Not needed anyway: every field is fully rewritten below. */
    UBRR0 = ubrr;
    UCSR0A = (1 << U2X0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00); /* 8N1 */
    UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
    g_initialized = 1;
    sei();
}

void hermes_uart_write_byte(uint8_t b) {
    if (!g_initialized) hermes_uart_init(HERMES_UART_DEFAULT_BAUD);
    while (!(UCSR0A & (1 << UDRE0))) {}
    UDR0 = b;
}

void hermes_uart_rx_disable(void) {
    /* only the RX bits: TX stays on (and the port is never written with
     * UCSR0B = 0 -- see the simavr note in hermes_uart_init) */
    UCSR0B &= (uint8_t)~((1 << RXEN0) | (1 << RXCIE0));
    g_rx_tail = g_rx_head; /* discard what was pending */
}

int hermes_uart_available(void) {
    return (uint8_t)(g_rx_head - g_rx_tail) & RX_MASK;
}

int hermes_uart_read_byte(void) {
    if (!g_initialized) hermes_uart_init(HERMES_UART_DEFAULT_BAUD);
    if (g_rx_head == g_rx_tail) return -1;
    uint8_t b = g_rx_buf[g_rx_tail];
    g_rx_tail = (uint8_t)((g_rx_tail + 1) & RX_MASK);
    return b;
}

/* Overrides hermes_stdlib.h's weak no-op: print() -> serial port. */
void hermes_putchar(char c) {
    hermes_uart_write_byte((uint8_t)c);
}

#else /* host */

#include <poll.h>
#include <stdio.h>
#include <unistd.h>

static int g_rx_enabled = 1;

void hermes_uart_init(uint32_t baud) {
    (void)baud;
    g_rx_enabled = 1;
}

void hermes_uart_rx_disable(void) {
    g_rx_enabled = 0;
}

void hermes_uart_write_byte(uint8_t b) {
    fputc(b, stdout);
}

/* poll() also reports "readable" at EOF, so availability can only be
 * known by actually reading: one byte read ahead is kept here. */
static int g_pending = -1;

static void fill_pending(void) {
    if (!g_rx_enabled) { g_pending = -1; return; }
    if (g_pending >= 0) return;
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
    unsigned char b;
    if (read(0, &b, 1) == 1) g_pending = b; /* 0 = EOF: nothing to read */
}

int hermes_uart_available(void) {
    fill_pending();
    return g_pending >= 0 ? 1 : 0;
}

int hermes_uart_read_byte(void) {
    fill_pending();
    int b = g_pending;
    g_pending = -1;
    return b;
}

#endif
