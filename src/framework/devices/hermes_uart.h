#ifndef HERMES_UART_H
#define HERMES_UART_H

/* `import hermes_uart`: the serial ports as built-in (core) devices.
 * hepyc links the UART driver (src/framework/drivers/hermes_drv_uart.c)
 * and the runtime automatically.
 *
 *   device serial = uart(9600)          port 0 (the one print() uses)
 *   device link = uart_port(1, 9600)    another port, where the chip has
 *                                       it (Mega 1-3, ESP32-C3 1; host 1-3:
 *                                       wires in memory, hermes_drv_uart.h)
 *   serial.write("hola")       text, no newline added
 *   serial.write_byte(65)      one raw byte
 *   long n = serial.available() bytes waiting
 *   long b = serial.read()      next byte, or -1 if none (never blocks)
 *   serial.port()               its port number
 *   serial.abort()              mandatory abort: stop receiving (TX stays,
 *                               so the program can still report why)
 *   serial.destroy()            abort, memory released, the variable left
 *                               empty; the port can be opened again. TX
 *                               stays on: print() uses port 0 on bare metal.
 *
 * A device holds its port's RX/TX pins (the board's
 * HERMES_BOARD_UART_PINS; Uno/Mega port 0: D0/D1): gpio(0) or gpio(1)
 * while it exists is a fatal error, and so is opening a port whose pins a
 * device already has, or a port that is already open, or one the chip
 * doesn't have. print() alone, with no uart device, holds nothing: print()
 * plus gpio(1) is the programmer's business. print() also goes out of port
 * 0 on AVR, with or without a uart device (see hermes_drv_uart.h). */

#include "hermes_device.h"
#include "hermes_drv_uart.h"
#include "hermes_sched_internal.h" /* hermes_runtime_alloc */
#include "hermes_diag.h"
#include <stdlib.h>

typedef struct {
    hermes_device_t device;
    long baud;
    signed char port;
} hermes_device_uart_t;

typedef hermes_handle_t hermes_device_uart_handle_t;
HERMES_DEVICE_KIND(uart);

/* Mandatory abort (hermes_device.h). A serial port isn't a hazard, and its
 * output is exactly how a program reports an emergency stop -- so abort
 * stops listening (no more incoming commands) but keeps transmitting. */
static inline void device_uart_abort(hermes_device_uart_handle_t h_u) {
    hermes_device_uart_t *u = HERMES_DEVICE_GET(hermes_device_uart_t, uart, h_u, "abort");
    hermes_uart_port_rx_disable(u->port);
}

#include "hermes_board.h" /* the ports' pins */
#ifndef HERMES_BOARD_UART_PINS
#define HERMES_BOARD_UART_PINS(X)
#endif

/* A port's pins on the board (-1: none listed, e.g. the host's wires). */
#define HERMES_UART_PIN_RX_(p, rx, tx) if (port == (p)) return (rx);
#define HERMES_UART_PIN_TX_(p, rx, tx) if (port == (p)) return (tx);
static inline int hermes_uart_pin_rx(int port) { HERMES_BOARD_UART_PINS(HERMES_UART_PIN_RX_) return -1; }
static inline int hermes_uart_pin_tx(int port) { HERMES_BOARD_UART_PINS(HERMES_UART_PIN_TX_) return -1; }

/* Mandatory describe (hermes_device.h): its two pins, memory and speed. */
static inline void device_uart_describe(hermes_device_uart_handle_t h_u, hermes_device_desc_t *o) {
    hermes_device_uart_t *u = HERMES_DEVICE_GET(hermes_device_uart_t, uart, h_u, "describe");
    o->pins[0] = (signed char)hermes_uart_pin_rx(u->port);
    o->pins[1] = (signed char)hermes_uart_pin_tx(u->port);
    o->bytes = sizeof(hermes_device_uart_t);
    HERMES_DEVICE_VALUE(o, "port", u->port);
    HERMES_DEVICE_VALUE(o, "baud", u->baud);
}

/* static via the macro: only the generated .c includes this header. One
 * device per port, at most 4 ports (the limit is checked per port). */
HERMES_DEVICE_CLASS(uart, hermes_device_uart_handle_t, 4);

/* `ctor`: "uart" or "uart_port" (HERMES_PSTR), for the messages. */
static inline hermes_device_uart_handle_t hermes_uart_open(long port, long baud, const char *ctor) {
    if (!hermes_uart_port_exists((int)port)) {
        HERMES_DIAG("hermes_uart: %" HERMES_PS "(%ld): this chip has no serial port %ld\n", ctor, port, port);
        hermes_fatal();
    }
    if (baud < 300 || baud > 2000000) {
        HERMES_DIAG("hermes_uart: %" HERMES_PS "(%ld): the speed must be between 300 and 2000000 baud\n", ctor, baud);
        hermes_fatal();
    }
    hermes_lock();
    for (hermes_device_t *it = hermes_device_class_uart.head; it; it = it->next) {
        if (((hermes_device_uart_t *)it)->port != port) continue;
        hermes_unlock();
        HERMES_DIAG("hermes_uart: %" HERMES_PS "(...): serial port %ld is already open\n", ctor, port);
        hermes_fatal();
    }
    hermes_device_pin_claim(hermes_uart_pin_rx((int)port), ctor, port);
    hermes_device_pin_claim(hermes_uart_pin_tx((int)port), ctor, port);
    hermes_device_uart_t *u = hermes_runtime_alloc(sizeof(hermes_device_uart_t));
    if (!u) {
        hermes_unlock();
        HERMES_DIAG("hermes_uart: %" HERMES_PS "(...): out of memory (%u bytes)\n", ctor, (unsigned)sizeof(hermes_device_uart_t));
        hermes_fatal();
    }
    u->baud = baud;
    u->port = (signed char)port;
    hermes_device_register_or_abort(&u->device, &hermes_device_class_uart);
    hermes_unlock();
    (void)ctor;
    hermes_uart_port_init((int)port, (uint32_t)baud);
    return hermes_device_handle(&u->device);
}

static inline hermes_device_uart_handle_t uart(long baud) {
    return hermes_uart_open(0, baud, HERMES_PSTR("uart"));
}

static inline hermes_device_uart_handle_t uart_port(long port, long baud) {
    return hermes_uart_open(port, baud, HERMES_PSTR("uart_port"));
}

/* destroy: only the abort (stop receiving); see above for TX. */
static inline void hermes_uart_release(hermes_device_t *d) {
    device_uart_abort(hermes_device_handle(d));
}

static inline void device_uart_destroy(hermes_device_uart_handle_t h_u) {
    hermes_device_uart_t *u = HERMES_DEVICE_GET(hermes_device_uart_t, uart, h_u, "destroy");
    hermes_device_destroy(&u->device, hermes_uart_release);
}

static inline void device_uart_write(hermes_device_uart_handle_t h_u, const char *s) {
    hermes_device_uart_t *u = HERMES_DEVICE_GET(hermes_device_uart_t, uart, h_u, "write");
    hermes_uart_port_write_str(u->port, s);
}

static inline void device_uart_write_byte(hermes_device_uart_handle_t h_u, long b) {
    hermes_device_uart_t *u = HERMES_DEVICE_GET(hermes_device_uart_t, uart, h_u, "write_byte");
    hermes_uart_port_write_byte(u->port, (uint8_t)b);
}

static inline long device_uart_available(hermes_device_uart_handle_t h_u) {
    hermes_device_uart_t *u = HERMES_DEVICE_GET(hermes_device_uart_t, uart, h_u, "available");
    return hermes_uart_port_available(u->port);
}

static inline long device_uart_read(hermes_device_uart_handle_t h_u) {
    hermes_device_uart_t *u = HERMES_DEVICE_GET(hermes_device_uart_t, uart, h_u, "read");
    return hermes_uart_port_read_byte(u->port);
}

static inline long device_uart_port(hermes_device_uart_handle_t h_u) {
    return HERMES_DEVICE_GET(hermes_device_uart_t, uart, h_u, "port")->port;
}

#endif
