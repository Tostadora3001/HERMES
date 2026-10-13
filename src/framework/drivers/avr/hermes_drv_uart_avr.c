/* UART driver, AVR implementation: USART0 (same registers on the
 * ATmega328P and the ATmega2560). See hermes_drv_uart.h. */
#include "hermes_drv_uart.h"
#include <avr/interrupt.h>
#include <avr/io.h>

#ifndef F_CPU
#error "hermes_drv_uart: F_CPU not defined (hepyc sets it from the board's f_cpu)"
#endif

static uint8_t g_initialized = 0;
static volatile uint8_t g_rx_buf[HERMES_UART_RX_BUFFER];
static volatile uint8_t g_rx_head = 0; /* written by the ISR only */
static volatile uint8_t g_rx_tail = 0; /* written by readers only */

#define RX_MASK ((uint8_t)(HERMES_UART_RX_BUFFER - 1))

/* the receive vector's name depends on the chip: USART_RX_vect on the
 * ATmega328P (one USART), USART0_RX_vect on the ATmega2560 (four) */
#if defined(USART0_RX_vect)
#define HERMES_UART_RX_VECT USART0_RX_vect
#else
#define HERMES_UART_RX_VECT USART_RX_vect
#endif

ISR(HERMES_UART_RX_VECT) {
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

/* "Room?" and the write with interrupts off: under hermes_os the tick may
 * switch threads in between (preemption), and another thread's byte would
 * take the room first -- writing UDR0 without room loses a byte. The wait
 * itself keeps interrupts on. */
void hermes_uart_write_byte(uint8_t b) {
    if (!g_initialized) hermes_uart_init(HERMES_UART_DEFAULT_BAUD);
    for (;;) {
        uint8_t sreg = SREG;
        cli();
        if (UCSR0A & (1 << UDRE0)) {
            UDR0 = b;
            SREG = sreg;
            return;
        }
        SREG = sreg;
    }
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

/* Interrupts off: two threads reading (one preempted between taking the
 * byte and moving the tail) would both get the same byte. */
int hermes_uart_read_byte(void) {
    if (!g_initialized) hermes_uart_init(HERMES_UART_DEFAULT_BAUD);
    int b = -1;
    uint8_t sreg = SREG;
    cli();
    if (g_rx_head != g_rx_tail) {
        b = g_rx_buf[g_rx_tail];
        g_rx_tail = (uint8_t)((g_rx_tail + 1) & RX_MASK);
    }
    SREG = sreg;
    return b;
}

/* Overrides hermes_stdlib.h's weak no-op: print() -> serial port. */
void hermes_putchar(char c) {
    hermes_uart_write_byte((uint8_t)c);
}
