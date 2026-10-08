#ifndef HERMES_UART_H
#define HERMES_UART_H

/* `import hermes_uart`: the serial port as a built-in (core) device.
 * hepyc links the UART driver (src/framework/drivers/hermes_drv_uart.c)
 * and the runtime automatically.
 *
 *   device serial = uart(9600)
 *   serial.write("hola")       text, no newline added
 *   serial.write_byte(65)      one raw byte
 *   long n = serial.available() bytes waiting
 *   long b = serial.read()      next byte, or -1 if none (never blocks)
 *   serial.abort()              mandatory abort: stop receiving (TX stays,
 *                               so the program can still report why)
 *
 * One instance at most (UART0 is the only one on an ATmega328P): a second
 * uart(...) aborts through the device registry's per-kind limit. print()
 * also goes out of this port on AVR, with or without a uart device (see
 * hermes_drv_uart.h). */

#include "hermes_device.h"
#include "hermes_drv_uart.h"
#include "hermes_sched_internal.h" /* hermes_runtime_alloc */
#include <stdlib.h>

typedef struct {
    hermes_device_t device;
    long baud;
} hermes_device_uart_t;

typedef hermes_device_uart_t *hermes_device_uart_handle_t;

/* Mandatory abort (hermes_device.h). A serial port isn't a hazard, and its
 * output is exactly how a program reports an emergency stop -- so abort
 * stops listening (no more incoming commands) but keeps transmitting. */
static inline void device_uart_abort(hermes_device_uart_handle_t u) {
    (void)u;
    hermes_uart_rx_disable();
}

/* static via the macro: only the generated .c includes this header. */
HERMES_DEVICE_CLASS(uart, hermes_device_uart_handle_t, 1);

static inline hermes_device_uart_handle_t uart(long baud) {
    hermes_device_uart_t *u = hermes_runtime_alloc(sizeof(hermes_device_uart_t));
    if (!u) hermes_fatal();
    hermes_device_register_or_abort(&u->device, &hermes_device_class_uart, "uart");
    u->baud = baud;
    hermes_uart_init((uint32_t)baud);
    return u;
}

static inline void device_uart_write(hermes_device_uart_handle_t u, const char *s) {
    (void)u;
    hermes_uart_write_str(s);
}

static inline void device_uart_write_byte(hermes_device_uart_handle_t u, long b) {
    (void)u;
    hermes_uart_write_byte((uint8_t)b);
}

static inline long device_uart_available(hermes_device_uart_handle_t u) {
    (void)u;
    return hermes_uart_available();
}

static inline long device_uart_read(hermes_device_uart_handle_t u) {
    (void)u;
    return hermes_uart_read_byte();
}

#endif
