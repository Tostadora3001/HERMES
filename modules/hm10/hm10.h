#ifndef HM10_H
#define HM10_H

/* `import hm10`: an HM-10 Bluetooth Low Energy module on a serial port
 * (asked by the user, 2026-10-16: phase 3 of the communications plan).
 * Once a central (a phone's app) connects, it's a transparent bridge.
 * The same methods as the hc05 module and a TCP connection:
 *
 *   device serial = uart_port(1, 9600)    the HM-10's port (factory: 9600)
 *   device ble = hm10(serial, "hermes")   its name (up to 12 characters);
 *                                         turns on its connection notices
 *   ble.connected()     a central is connected ("OK+CONN" / "OK+LOST")
 *   ble.send_text(s), ble.send_byte(b)    False when not connected
 *   ble.available(), ble.read(), ble.read_wait(ms)
 *   ble.abort() / ble.destroy()           stops using it
 *
 * Set up with AT commands at the start (the module must not be connected
 * yet: while connected, it passes "AT" on as data); an HM-10 that doesn't
 * answer is a fatal error with its message. The notices come in the same
 * stream as the data, so the module takes "OK+CONN" and "OK+LOST" out of
 * it -- data that is exactly one of them would be taken too, and a
 * received "O", "OK"... waits up to 20 ms in case a notice follows. On the
 * host the HM-10 is a virtual one (modules/hc05/bt_virtual.c) on the UART
 * mock's port. See docs/raw/hermes_bluetooth.en.md. */

#include "hermes_device.h"
#include "hermes_sched_internal.h"
#include "hermes_diag.h"
#include "hermes_uart.h"
#include <stdbool.h>
#include <stdint.h>

#ifndef HERMES_HM10_RX
#if defined(__AVR__)
#define HERMES_HM10_RX 64
#else
#define HERMES_HM10_RX 512
#endif
#endif

typedef struct {
    hermes_device_t device;
    signed char port;
    unsigned char live, connected;
    unsigned char hold_len;
    char hold[8];        /* bytes that may start a notice */
    uint32_t hold_ms;    /* when the last of them came */
    uint16_t head, tail; /* the data that passed the filter */
    uint8_t rx[HERMES_HM10_RX];
} hermes_device_hm10_t;

typedef hermes_handle_t hermes_device_hm10_handle_t;
HERMES_DEVICE_KIND(hm10);

static inline void device_hm10_abort(hermes_device_hm10_handle_t h) {
    hermes_device_hm10_t *b = HERMES_DEVICE_GET(hermes_device_hm10_t, hm10, h, "abort");
    b->live = 0;
}

static inline void device_hm10_describe(hermes_device_hm10_handle_t h, hermes_device_desc_t *o) {
    hermes_device_hm10_t *b = HERMES_DEVICE_GET(hermes_device_hm10_t, hm10, h, "describe");
    o->bytes = sizeof(hermes_device_hm10_t);
    HERMES_DEVICE_VALUE(o, "port", b->port); (void)b;
}

HERMES_DEVICE_CLASS(hm10, hermes_device_hm10_handle_t, 4);

/* hm10.c */
bool hermes_hm10_setup(hermes_device_hm10_t *b, const char *name);
bool hermes_hm10_connected(hermes_device_hm10_t *b);
bool hermes_hm10_send(hermes_device_hm10_t *b, const char *s, int n);
long hermes_hm10_available(hermes_device_hm10_t *b);
long hermes_hm10_read(hermes_device_hm10_t *b, long wait_ms);

static inline hermes_device_hm10_handle_t hm10(hermes_device_uart_handle_t serial, const char *name) {
    int port = (int)device_uart_port(serial);
    hermes_lock();
    hermes_device_hm10_t *b = hermes_runtime_alloc(sizeof(hermes_device_hm10_t));
    if (!b) {
        hermes_unlock();
        HERMES_DIAG("hm10: out of memory (%u bytes)\n", (unsigned)sizeof(hermes_device_hm10_t));
        hermes_fatal();
    }
    b->port = (signed char)port;
    b->live = 1;
    b->connected = 0;
    b->hold_len = 0;
    b->head = b->tail = 0;
    hermes_device_register_or_abort(&b->device, &hermes_device_class_hm10);
    hermes_unlock();
    if (!hermes_hm10_setup(b, name)) {
        HERMES_DIAG("hm10: no HM-10 answers on serial port %d\n", port);
        hermes_fatal();
    }
    return hermes_device_handle(&b->device);
}

static inline bool device_hm10_connected(hermes_device_hm10_handle_t h) {
    return hermes_hm10_connected(HERMES_DEVICE_GET(hermes_device_hm10_t, hm10, h, "connected"));
}

static inline bool device_hm10_send_text(hermes_device_hm10_handle_t h, const char *s) {
    int n = 0;
    while (s[n]) n++;
    return hermes_hm10_send(HERMES_DEVICE_GET(hermes_device_hm10_t, hm10, h, "send_text"), s, n);
}

static inline bool device_hm10_send_byte(hermes_device_hm10_handle_t h, long c) {
    char x = (char)c;
    return hermes_hm10_send(HERMES_DEVICE_GET(hermes_device_hm10_t, hm10, h, "send_byte"), &x, 1);
}

static inline long device_hm10_available(hermes_device_hm10_handle_t h) {
    return hermes_hm10_available(HERMES_DEVICE_GET(hermes_device_hm10_t, hm10, h, "available"));
}

static inline long device_hm10_read(hermes_device_hm10_handle_t h) {
    return hermes_hm10_read(HERMES_DEVICE_GET(hermes_device_hm10_t, hm10, h, "read"), 0);
}

static inline long device_hm10_read_wait(hermes_device_hm10_handle_t h, long ms) {
    return hermes_hm10_read(HERMES_DEVICE_GET(hermes_device_hm10_t, hm10, h, "read_wait"), ms);
}

static inline void device_hm10_destroy(hermes_device_hm10_handle_t h) {
    hermes_device_hm10_t *b = HERMES_DEVICE_GET(hermes_device_hm10_t, hm10, h, "destroy");
    hermes_device_destroy(&b->device, 0);
}

#endif
