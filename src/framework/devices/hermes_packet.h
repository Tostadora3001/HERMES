#ifndef HERMES_PACKET_H
#define HERMES_PACKET_H

/* `import hermes_packet`: reliable packets over a serial port (asked by
 * the user, 2026-10-16: frames with a CRC, acknowledged, resent when lost
 * -- delivered once and in order between two boards). A core device; hepyc
 * links hermes_packet.c, the UART and timer drivers and the runtime.
 *
 *   device link = packet_link(1, 57600)  opens serial port 1 (a uart
 *                                        device, see hermes_uart.h) and
 *                                        speaks the protocol on it
 *   link.send_text("hola")      a packet with that text; waits for its
 *                               ACK, resending: True if it arrived
 *   link.put(b) / link.put_long(v) / link.put_text(s)
 *                               build a packet (bytes; a long as 4 bytes,
 *                               little-endian; text without its 0) ...
 *   link.send()                 ... and send it (True if it arrived)
 *   long n = link.receive()     next packet's length, -1 if none yet
 *                               (never blocks); it stays readable with:
 *   link.get(i) / link.get_long(i) / link.text()
 *                               byte i / the long at byte i / the packet
 *                               as text
 *   long n = link.receive_wait(ms)  the same, waiting up to ms ms
 *   link.error()                "ok", "no_ack" (every try lost),
 *                               "too_long", "empty"
 *   link.timeout(ms) / link.retries(n)   wait per try (default from
 *                               the speed) / tries after the first (5)
 *   link.resends()              packets sent again so far (statistics)
 *   link.abort() / link.destroy()   stop receiving / close it (its port
 *                               too)
 *
 * Packets of up to HERMES_PACKET_MAX bytes (32 on AVR, 128 elsewhere).
 * Who answers: an ACK goes out when the receiving side reads its port --
 * every 1 ms on its own under hermes_os (the kind's device tick), and on
 * every call to the link's methods. Without the OS, a program must call
 * receive() (or any method) often enough on the receiving board, or the
 * sender's tries run out. One packet waits to be read at a time: until
 * receive() takes it, the next one isn't acknowledged, so its sender
 * waits and resends (flow control, nothing lost). The wire format and the
 * rules: docs/raw/hermes_packet.en.md. */

#include "hermes_device.h"
#include "hermes_uart.h"
#include "hermes_diag.h"
#include <stdbool.h>
#include <stdint.h>

#ifndef HERMES_PACKET_MAX
#if defined(__AVR__)
#define HERMES_PACKET_MAX 32
#else
#define HERMES_PACKET_MAX 128
#endif
#endif

/* The protocol's state (hermes_packet.c). */
typedef struct hermes_packet_state {
    signed char port;
    volatile unsigned char busy;   /* one user of the link at a time */
    unsigned char err;             /* HERMES_PACKET_ERR_* */
    unsigned char tx_seq;          /* sequence of the packet being sent */
    volatile unsigned char acked;  /* its ACK came */
    unsigned char tx_len;
    unsigned char tx[HERMES_PACKET_MAX];
    /* receiving: the frame being assembled */
    unsigned char rx_state, rx_esc, rx_type, rx_seq, rx_len, rx_got;
    unsigned char rx[HERMES_PACKET_MAX + 2]; /* payload + CRC */
    /* the packet delivered and waiting to be read */
    unsigned char in_len, in_ready, in_read, have_last, last_seq;
    unsigned char in[HERMES_PACKET_MAX + 1]; /* + a 0 for text() */
    unsigned short timeout_ms;
    unsigned char retries;
    unsigned long resends;
} hermes_packet_state_t;

enum { HERMES_PACKET_ERR_OK, HERMES_PACKET_ERR_NO_ACK, HERMES_PACKET_ERR_TOO_LONG,
       HERMES_PACKET_ERR_EMPTY };

void hermes_packet_init(hermes_packet_state_t *p, int port, long baud);
void hermes_packet_pump(hermes_packet_state_t *p);
bool hermes_packet_put(hermes_packet_state_t *p, const unsigned char *b, int n);
bool hermes_packet_send(hermes_packet_state_t *p);
long hermes_packet_receive(hermes_packet_state_t *p, long wait_ms);
long hermes_packet_get(hermes_packet_state_t *p, long i, int bytes);
const char *hermes_packet_text(hermes_packet_state_t *p);
const char *hermes_packet_error(hermes_packet_state_t *p);

typedef struct {
    hermes_device_t device;
    hermes_device_uart_handle_t uart; /* its port, a uart device */
    hermes_packet_state_t st;
} hermes_device_packet_t;

typedef hermes_handle_t hermes_device_packet_handle_t;
HERMES_DEVICE_KIND(packet);

#define HERMES_PACKET_GET(h, method) (&HERMES_DEVICE_GET(hermes_device_packet_t, packet, h, method)->st)

/* Mandatory abort: stop receiving (its port's RX off). */
static inline void device_packet_abort(hermes_device_packet_handle_t h) {
    hermes_packet_state_t *p = HERMES_PACKET_GET(h, "abort");
    hermes_uart_port_rx_disable(p->port);
}

/* Mandatory describe: no pins of its own (its uart device holds them). */
static inline void device_packet_describe(hermes_device_packet_handle_t h, hermes_device_desc_t *o) {
    hermes_packet_state_t *p = HERMES_PACKET_GET(h, "describe");
    o->bytes = sizeof(hermes_device_packet_t);
    HERMES_DEVICE_VALUE(o, "port", p->port);
    HERMES_DEVICE_VALUE(o, "resends", (long)p->resends); (void)p;
}

/* Device tick (the OS, every ms): answer the other side. */
static inline void device_packet_tick(hermes_device_packet_handle_t h) {
    hermes_packet_pump(HERMES_PACKET_GET(h, "tick"));
}

HERMES_DEVICE_CLASS_TICK(packet, hermes_device_packet_handle_t, 4, 1);

static inline hermes_device_packet_handle_t packet_link(long port, long baud) {
    hermes_device_uart_handle_t u = hermes_uart_open(port, baud, HERMES_PSTR("packet_link"));
    hermes_lock();
    hermes_device_packet_t *l = hermes_runtime_alloc(sizeof(hermes_device_packet_t));
    if (!l) {
        hermes_unlock();
        HERMES_DIAG("hermes_packet: packet_link(...): out of memory (%u bytes)\n", (unsigned)sizeof(hermes_device_packet_t));
        hermes_fatal();
    }
    l->uart = u;
    hermes_packet_init(&l->st, (int)port, baud); /* ready before the tick can see it */
    hermes_device_register_or_abort(&l->device, &hermes_device_class_packet);
    hermes_unlock();
    return hermes_device_handle(&l->device);
}

/* destroy: the link, then its port */
static inline void hermes_packet_release(hermes_device_t *d) {
    device_packet_abort(hermes_device_handle(d));
}

static inline void device_packet_destroy(hermes_device_packet_handle_t h) {
    hermes_device_packet_t *l = HERMES_DEVICE_GET(hermes_device_packet_t, packet, h, "destroy");
    hermes_device_uart_handle_t u = l->uart;
    hermes_device_destroy(&l->device, hermes_packet_release);
    device_uart_destroy(u);
}

static inline bool device_packet_send_text(hermes_device_packet_handle_t h, const char *s) {
    hermes_packet_state_t *p = HERMES_PACKET_GET(h, "send_text");
    int n = 0;
    while (s[n]) n++;
    p->tx_len = 0;
    return hermes_packet_put(p, (const unsigned char *)s, n) && hermes_packet_send(p);
}

static inline bool device_packet_put(hermes_device_packet_handle_t h, long b) {
    unsigned char c = (unsigned char)b;
    return hermes_packet_put(HERMES_PACKET_GET(h, "put"), &c, 1);
}

static inline bool device_packet_put_long(hermes_device_packet_handle_t h, long v) {
    unsigned char c[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                           (unsigned char)(v >> 24) };
    return hermes_packet_put(HERMES_PACKET_GET(h, "put_long"), c, 4);
}

static inline bool device_packet_put_text(hermes_device_packet_handle_t h, const char *s) {
    int n = 0;
    while (s[n]) n++;
    return hermes_packet_put(HERMES_PACKET_GET(h, "put_text"), (const unsigned char *)s, n);
}

static inline bool device_packet_send(hermes_device_packet_handle_t h) {
    return hermes_packet_send(HERMES_PACKET_GET(h, "send"));
}

static inline long device_packet_receive(hermes_device_packet_handle_t h) {
    return hermes_packet_receive(HERMES_PACKET_GET(h, "receive"), 0);
}

static inline long device_packet_receive_wait(hermes_device_packet_handle_t h, long ms) {
    return hermes_packet_receive(HERMES_PACKET_GET(h, "receive_wait"), ms);
}

static inline long device_packet_get(hermes_device_packet_handle_t h, long i) {
    return hermes_packet_get(HERMES_PACKET_GET(h, "get"), i, 1);
}

static inline long device_packet_get_long(hermes_device_packet_handle_t h, long i) {
    return hermes_packet_get(HERMES_PACKET_GET(h, "get_long"), i, 4);
}

static inline const char *device_packet_text(hermes_device_packet_handle_t h) {
    return hermes_packet_text(HERMES_PACKET_GET(h, "text"));
}

static inline const char *device_packet_error(hermes_device_packet_handle_t h) {
    return hermes_packet_error(HERMES_PACKET_GET(h, "error"));
}

static inline void device_packet_timeout(hermes_device_packet_handle_t h, long ms) {
    hermes_packet_state_t *p = HERMES_PACKET_GET(h, "timeout");
    p->timeout_ms = (unsigned short)(ms < 1 ? 1 : ms > 65535 ? 65535 : ms);
}

static inline void device_packet_retries(hermes_device_packet_handle_t h, long n) {
    hermes_packet_state_t *p = HERMES_PACKET_GET(h, "retries");
    p->retries = (unsigned char)(n < 0 ? 0 : n > 255 ? 255 : n);
}

static inline long device_packet_resends(hermes_device_packet_handle_t h) {
    return (long)HERMES_PACKET_GET(h, "resends")->resends;
}

#endif
