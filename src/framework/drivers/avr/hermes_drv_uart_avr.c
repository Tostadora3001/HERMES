/* UART driver, AVR implementation: every USART of the chip -- USART0 on
 * the ATmega328P; USART0-3 on the ATmega2560 (same registers and bit
 * positions for each, numbered 0-3). See hermes_drv_uart.h. */
#include "hermes_drv_uart.h"
#include <avr/interrupt.h>
#include <avr/io.h>

#ifndef F_CPU
#error "hermes_drv_uart: F_CPU not defined (hepyc sets it from the board's f_cpu)"
#endif

#if defined(UDR3)
#define HERMES_UART_HW_PORTS 4
#elif defined(UDR2)
#define HERMES_UART_HW_PORTS 3
#elif defined(UDR1)
#define HERMES_UART_HW_PORTS 2
#else
#define HERMES_UART_HW_PORTS 1
#endif

#define RX_MASK ((uint8_t)(HERMES_UART_RX_BUFFER - 1))

/* Per port: ring buffer filled by its RX interrupt. head is written by the
 * interrupt only, tail by readers only. */
static struct {
    volatile uint8_t buf[HERMES_UART_RX_BUFFER];
    volatile uint8_t head, tail;
    uint8_t initialized;
} g_port[HERMES_UART_HW_PORTS];

/* A port's registers. Every USART has the same bits at the same
 * positions: the USART0 names (U2X0, RXEN0...) serve for all. */
typedef struct {
    volatile uint8_t *ucsra, *ucsrb, *ucsrc, *udr;
    volatile uint16_t *ubrr;
} hermes_usart_t;

static hermes_usart_t usart(int port) {
    switch (port) {
#if HERMES_UART_HW_PORTS > 1
    case 1: return (hermes_usart_t){ &UCSR1A, &UCSR1B, &UCSR1C, &UDR1, &UBRR1 };
#endif
#if HERMES_UART_HW_PORTS > 2
    case 2: return (hermes_usart_t){ &UCSR2A, &UCSR2B, &UCSR2C, &UDR2, &UBRR2 };
#endif
#if HERMES_UART_HW_PORTS > 3
    case 3: return (hermes_usart_t){ &UCSR3A, &UCSR3B, &UCSR3C, &UDR3, &UBRR3 };
#endif
    default: return (hermes_usart_t){ &UCSR0A, &UCSR0B, &UCSR0C, &UDR0, &UBRR0 };
    }
}

static inline void rx_byte(uint8_t port, uint8_t b) {
    uint8_t next = (uint8_t)((g_port[port].head + 1) & RX_MASK);
    if (next != g_port[port].tail) {
        g_port[port].buf[g_port[port].head] = b;
        g_port[port].head = next;
    } /* else: buffer full, byte dropped */
}

/* the receive vectors: USART_RX_vect on the ATmega328P (one USART),
 * USARTn_RX_vect on the ATmega2560 (four). UDRn is always read: that
 * clears the interrupt. */
#if defined(USART0_RX_vect)
ISR(USART0_RX_vect) { rx_byte(0, UDR0); }
#else
ISR(USART_RX_vect) { rx_byte(0, UDR0); }
#endif
#if HERMES_UART_HW_PORTS > 1
ISR(USART1_RX_vect) { rx_byte(1, UDR1); }
#endif
#if HERMES_UART_HW_PORTS > 2
ISR(USART2_RX_vect) { rx_byte(2, UDR2); }
#endif
#if HERMES_UART_HW_PORTS > 3
ISR(USART3_RX_vect) { rx_byte(3, UDR3); }
#endif

int hermes_uart_port_exists(int port) {
    return port >= 0 && port < HERMES_UART_HW_PORTS;
}

void hermes_uart_port_init(int port, uint32_t baud) {
    if (!hermes_uart_port_exists(port)) return;
    hermes_usart_t u = usart(port);
    /* double speed: UBRR = F_CPU / (8 * baud) - 1, rounded to nearest */
    uint16_t ubrr = (uint16_t)((F_CPU + 4UL * baud) / (8UL * baud) - 1UL);
    if (g_port[port].initialized) {
        while (!(*u.ucsra & (1 << UDRE0))) {} /* let the last byte leave */
    }
    /* No `UCSRnB = 0` first to "reset" the port: harmless on real
     * hardware, but under simavr it leaves UDRE permanently clear, so the
     * first write_byte waits forever (found while verifying this driver).
     * Not needed anyway: every field is fully rewritten below. */
    *u.ubrr = ubrr;
    *u.ucsra = (1 << U2X0);
    *u.ucsrc = (1 << UCSZ01) | (1 << UCSZ00); /* 8N1 */
    *u.ucsrb = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
    g_port[port].initialized = 1;
    sei();
}

/* "Room?" and the write with interrupts off: under hermes_os the tick may
 * switch threads in between (preemption), and another thread's byte would
 * take the room first -- writing UDRn without room loses a byte. The wait
 * itself keeps interrupts on. */
void hermes_uart_port_write_byte(int port, uint8_t b) {
    if (!hermes_uart_port_exists(port)) return;
    if (!g_port[port].initialized) hermes_uart_port_init(port, HERMES_UART_DEFAULT_BAUD);
    hermes_usart_t u = usart(port);
    for (;;) {
        uint8_t sreg = SREG;
        cli();
        if (*u.ucsra & (1 << UDRE0)) {
            *u.udr = b;
            SREG = sreg;
            return;
        }
        SREG = sreg;
    }
}

void hermes_uart_port_rx_disable(int port) {
    if (!hermes_uart_port_exists(port)) return;
    /* only the RX bits: TX stays on (and the port is never written with
     * UCSRnB = 0 -- see the simavr note in hermes_uart_port_init) */
    *usart(port).ucsrb &= (uint8_t)~((1 << RXEN0) | (1 << RXCIE0));
    g_port[port].tail = g_port[port].head; /* discard what was pending */
}

int hermes_uart_port_available(int port) {
    if (!hermes_uart_port_exists(port)) return 0;
    return (uint8_t)(g_port[port].head - g_port[port].tail) & RX_MASK;
}

/* Interrupts off: two threads reading (one preempted between taking the
 * byte and moving the tail) would both get the same byte. */
int hermes_uart_port_read_byte(int port) {
    if (!hermes_uart_port_exists(port)) return -1;
    if (!g_port[port].initialized) hermes_uart_port_init(port, HERMES_UART_DEFAULT_BAUD);
    int b = -1;
    uint8_t sreg = SREG;
    cli();
    if (g_port[port].head != g_port[port].tail) {
        b = g_port[port].buf[g_port[port].tail];
        g_port[port].tail = (uint8_t)((g_port[port].tail + 1) & RX_MASK);
    }
    SREG = sreg;
    return b;
}

/* host-only (the wires in memory) */
void hermes_uart_mock_faults(int drop_every, int flip_every) { (void)drop_every; (void)flip_every; }
void hermes_uart_mock_attach(int port, const hermes_uart_mock_device_t *dev) { (void)port; (void)dev; }

/* Overrides hermes_stdlib.h's weak no-op: print() -> serial port 0. */
void hermes_putchar(char c) {
    hermes_uart_port_write_byte(0, (uint8_t)c);
}
