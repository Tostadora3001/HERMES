#ifndef HC05_H
#define HC05_H

/* `import hc05`: an HC-05 Bluetooth module (classic, serial port profile)
 * on a serial port (asked by the user, 2026-10-16: phase 3 of the
 * communications plan). The HC-05 is a transparent bridge: what the
 * program writes comes out at the paired device (a phone, a PC) and the
 * other way round. Its methods are those of a TCP connection
 * (hermes_net), so a program changes its link by changing one line:
 *
 *   device serial = uart_port(1, 9600)   the HC-05's port (its factory
 *                                        speed in data mode: 9600)
 *   device bt = hc05(serial, 7)          7: its STATE pin (high while a
 *                                        device is connected); -1: not wired
 *   bt.connected()      the STATE pin; without it, always True
 *   bt.send_text(s), bt.send_byte(b)     False when not connected
 *   bt.available(), bt.read(), bt.read_wait(ms)
 *   bt.abort() / bt.destroy()            stops using it (the HC-05 itself
 *                                        keeps its link: it has no command
 *                                        for that in data mode)
 *
 * Its name, PIN and speed are set in its AT mode (KEY pin high at power
 * up, 38400 baud): not done by this module. On the host the HC-05 is a
 * virtual one (bt_virtual.c) on the UART mock's port, paired to a device
 * that echoes back what it gets. See docs/raw/hermes_bluetooth.en.md. */

#include "hermes_device.h"
#include "hermes_sched_internal.h"
#include "hermes_diag.h"
#include "hermes_drv_uart.h"
#include "hermes_drv_gpio.h"
#include "hermes_drv_timer.h"
#include "hermes_uart.h"
#include <stdbool.h>

typedef struct {
    hermes_device_t device;
    signed char port;
    signed char state;  /* STATE pin, -1: none */
    unsigned char live; /* 0 after abort */
} hermes_device_hc05_t;

typedef hermes_handle_t hermes_device_hc05_handle_t;
HERMES_DEVICE_KIND(hc05);

/* Mandatory abort: the program stops using it (a radio link isn't a
 * hazard, and the HC-05 can't drop it in data mode). */
static inline void device_hc05_abort(hermes_device_hc05_handle_t h) {
    hermes_device_hc05_t *b = HERMES_DEVICE_GET(hermes_device_hc05_t, hc05, h, "abort");
    b->live = 0;
}

/* Mandatory describe: its STATE pin (the serial port's pins are the uart
 * device's). */
static inline void device_hc05_describe(hermes_device_hc05_handle_t h, hermes_device_desc_t *o) {
    hermes_device_hc05_t *b = HERMES_DEVICE_GET(hermes_device_hc05_t, hc05, h, "describe");
    o->pins[0] = b->state;
    o->bytes = sizeof(hermes_device_hc05_t);
    HERMES_DEVICE_VALUE(o, "port", b->port);
}

HERMES_DEVICE_CLASS(hc05, hermes_device_hc05_handle_t, 4);

/* weak: on the host, hc05_mock.c puts a virtual HC-05 on that port */
extern void hermes_hc05_mock_attach(int port, int state_pin) __attribute__((weak));

static inline hermes_device_hc05_handle_t hc05(hermes_device_uart_handle_t serial, long state_pin) {
    int port = (int)device_uart_port(serial);
    hermes_lock();
    if (state_pin >= 0) hermes_device_pin_claim((int)state_pin, HERMES_PSTR("hc05"), state_pin);
    hermes_device_hc05_t *b = hermes_runtime_alloc(sizeof(hermes_device_hc05_t));
    if (!b) {
        hermes_unlock();
        HERMES_DIAG("hc05: out of memory (%u bytes)\n", (unsigned)sizeof(hermes_device_hc05_t));
        hermes_fatal();
    }
    b->port = (signed char)port;
    b->state = (signed char)(state_pin >= 0 ? state_pin : -1);
    b->live = 1;
    hermes_device_register_or_abort(&b->device, &hermes_device_class_hc05);
    hermes_unlock();
    if (b->state >= 0) hermes_gpio_mode(b->state, HERMES_GPIO_INPUT);
    if (hermes_hc05_mock_attach) hermes_hc05_mock_attach(port, b->state);
    return hermes_device_handle(&b->device);
}

static inline bool device_hc05_connected(hermes_device_hc05_handle_t h) {
    hermes_device_hc05_t *b = HERMES_DEVICE_GET(hermes_device_hc05_t, hc05, h, "connected");
    return b->live && (b->state < 0 || hermes_gpio_read(b->state));
}

static inline bool device_hc05_send_byte(hermes_device_hc05_handle_t h, long c) {
    if (!device_hc05_connected(h)) return false;
    hermes_device_hc05_t *b = HERMES_DEVICE_GET(hermes_device_hc05_t, hc05, h, "send_byte");
    hermes_uart_port_write_byte(b->port, (uint8_t)c);
    return true;
}

static inline bool device_hc05_send_text(hermes_device_hc05_handle_t h, const char *s) {
    if (!device_hc05_connected(h)) return false;
    hermes_device_hc05_t *b = HERMES_DEVICE_GET(hermes_device_hc05_t, hc05, h, "send_text");
    while (*s) hermes_uart_port_write_byte(b->port, (uint8_t)*s++);
    return true;
}

static inline long device_hc05_available(hermes_device_hc05_handle_t h) {
    hermes_device_hc05_t *b = HERMES_DEVICE_GET(hermes_device_hc05_t, hc05, h, "available");
    return b->live ? hermes_uart_port_available(b->port) : 0;
}

static inline long device_hc05_read(hermes_device_hc05_handle_t h) {
    hermes_device_hc05_t *b = HERMES_DEVICE_GET(hermes_device_hc05_t, hc05, h, "read");
    return b->live ? hermes_uart_port_read_byte(b->port) : -1;
}

/* waits 1 ms at a time (under hermes_os the thread sleeps meanwhile) */
static inline long device_hc05_read_wait(hermes_device_hc05_handle_t h, long ms) {
    hermes_device_hc05_t *b = HERMES_DEVICE_GET(hermes_device_hc05_t, hc05, h, "read_wait");
    uint32_t start = hermes_timer_millis();
    for (;;) {
        int c = b->live ? hermes_uart_port_read_byte(b->port) : -1;
        if (c >= 0 || !b->live || (long)(hermes_timer_millis() - start) >= ms) return c;
        hermes_timer_delay_ms(1);
    }
}

static inline void hermes_hc05_release(hermes_device_t *d) {
    hermes_device_hc05_t *b = (hermes_device_hc05_t *)d;
    b->live = 0; /* its STATE pin stays an input: free for another device */
}

static inline void device_hc05_destroy(hermes_device_hc05_handle_t h) {
    hermes_device_hc05_t *b = HERMES_DEVICE_GET(hermes_device_hc05_t, hc05, h, "destroy");
    hermes_device_destroy(&b->device, hermes_hc05_release);
}

#endif
