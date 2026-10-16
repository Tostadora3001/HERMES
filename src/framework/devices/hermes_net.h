#ifndef HERMES_NET_H
#define HERMES_NET_H

/* `import hermes_net`: TCP and UDP sockets (asked by the user,
 * 2026-10-16: phase 2 of the communications plan). Decided with the user:
 * the IP stack is the network hardware's (no TCP/IP of HERMES's own); this
 * core library is the common API, and each piece of hardware is an
 * "interface" behind it -- a module (modules/w5500: the W5500 Ethernet
 * chip) or, on the host, the system's own sockets (net_host()). Scope:
 * TCP client and UDP.
 *
 *   device eth = w5500(chip, "192.168.1.50")   an interface (a module)
 *   device eth = net_host()                    host only: the PC's sockets
 *
 *   device c = eth.tcp("192.168.1.10", 7)   TCP connection (waits up to
 *                                           HERMES_NET_CONNECT_MS for it)
 *   c.connected()          still connected?
 *   c.send_text(s)         text (no 0), True if it went out whole
 *   c.send_byte(b)
 *   long n = c.available() bytes waiting
 *   long b = c.read()      next byte, -1 if none (never blocks)
 *   long b = c.read_wait(ms)  the same, waiting up to ms
 *   c.destroy()            closes it
 *
 *   device u = eth.udp(9000)               UDP socket on that local port
 *   u.send_to("192.168.1.10", 7, s)        a datagram with that text
 *   long n = u.receive()                   next datagram's length, -1 if
 *   long n = u.receive_wait(ms)            none (it stays readable with:)
 *   u.get(i) / u.text() / u.from_ip() / u.from_port()
 *   u.destroy()
 *
 *   device srv = eth.listen(80)            TCP server on that port
 *   srv.waiting()                          a client is connected, to accept
 *   device c = srv.accept()                that client, as a tcp device
 *                                          (not connected if none waits)
 *
 *   eth.dhcp()           an address (mask, gateway, DNS) from DHCP: True if
 *                        it got one (an interface whose module does DHCP
 *                        itself: True if it has an address)
 *   eth.link()           the interface is up (cable / Wi-Fi joined)
 *   eth.ip()             its own address, as text ("" if it has none)
 *   eth.resolve(name)    DNS: the name's address as text, "" if unknown
 *   eth.dns(ip)          the DNS server to use
 *
 * Addresses are written as text: "a.b.c.d", or a name the interface
 * resolves by DNS ("example.com"). Failures to resolve, connect or send
 * are returned (False / -1), never fatal: a network goes down.
 *
 * The server, DNS and DHCP are compiled in only if the program uses them
 * (hepyc: -DHERMES_NET_SERVER when it calls listen(), -DHERMES_NET_DNS
 * when resolve()/dns() or a host name -- anything but a literal "a.b.c.d"
 * -- in tcp()/send_to(), -DHERMES_NET_DHCP when dhcp()): on an AVR they
 * are a few KB of flash. Each interface leaves those operations NULL
 * without its flag. Datagrams of up to HERMES_NET_DGRAM_MAX bytes (64 on
 * AVR, 512 elsewhere); a longer one is cut. See docs/raw/hermes_net.en.md. */

#include "hermes_device.h"
#include "hermes_sched_internal.h"
#include "hermes_diag.h"
#include <stdbool.h>
#include <stdint.h>

#ifndef HERMES_NET_DGRAM_MAX
#if defined(__AVR__)
#define HERMES_NET_DGRAM_MAX 64
#else
#define HERMES_NET_DGRAM_MAX 512
#endif
#endif
#ifndef HERMES_NET_CONNECT_MS
#define HERMES_NET_CONNECT_MS 3000
#endif

/* A socket's state, as an interface reports it. */
enum { HERMES_NET_CLOSED = 0, HERMES_NET_CONNECTING, HERMES_NET_CONNECTED, HERMES_NET_OPEN_UDP };

/* What an interface does (a module fills one; all called by one thread
 * at a time per interface -- hermes_net.c serializes them). Sockets are
 * the interface's own small numbers. */
typedef struct hermes_net_ops {
    int (*tcp_open)(void *nif, const uint8_t ip[4], uint16_t port);   /* starts connecting: socket, or -1 */
    int (*udp_open)(void *nif, uint16_t port);                        /* socket, or -1 */
    int (*status)(void *nif, int s);                                  /* HERMES_NET_* */
    int (*send)(void *nif, int s, const uint8_t *b, int n);           /* bytes sent, -1 */
    int (*send_to)(void *nif, int s, const uint8_t ip[4], uint16_t port, const uint8_t *b, int n);
    int (*available)(void *nif, int s);                               /* TCP: bytes waiting */
    int (*recv)(void *nif, int s, uint8_t *b, int max);               /* TCP: bytes read */
    int (*recv_from)(void *nif, int s, uint8_t *b, int max, uint8_t ip[4], uint16_t *port); /* UDP: a datagram's length, -1 none */
    void (*close)(void *nif, int s);
    void (*down)(void *nif);                                          /* abort: every socket closed */
    /* optional (NULL: not offered by this interface) */
    int (*listen)(void *nif, uint16_t port);                          /* a TCP server: its handle, -1 */
    int (*accept)(void *nif, int srv, int take);                      /* a client's socket waiting, -1 none;
                                                                         take = 0: only look */
    void (*unlisten)(void *nif, int srv);
    int (*link)(void *nif);                                           /* 1: the interface is up */
    int (*address)(void *nif, uint8_t ip[4]);                         /* 1: its own address in ip */
    int (*resolve)(void *nif, const char *name, uint8_t ip[4]);       /* DNS: 1 if found */
    int (*set_dns)(void *nif, const uint8_t ip[4]);
    int (*dhcp)(void *nif);                                           /* 1: an address from DHCP */
} hermes_net_ops_t;

/* --- hermes_net.c ------------------------------------------------------------ */
bool hermes_net_parse_ip(const char *text, uint8_t ip[4]);
const char *hermes_net_ip_text(const uint8_t ip[4], char out[16]);
void hermes_net_lock(void);
void hermes_net_unlock(void);

/* --- the interface device ------------------------------------------------------ */

typedef struct {
    hermes_device_t device;
    const hermes_net_ops_t *ops;
    void *nif;         /* the interface's own state */
    const char *what;  /* "w5500", "host"... (for the dump) */
} hermes_device_net_t;

typedef struct {
    hermes_device_t device;
    hermes_device_net_t *net;
    signed char sock;
    uint8_t ip[4];
    uint16_t port;
} hermes_device_tcp_t;

typedef struct {
    hermes_device_t device;
    hermes_device_net_t *net;
    signed char sock;
    uint16_t port;
    int len;           /* the last datagram received (-1: none) */
    uint8_t from[4];
    uint16_t from_port;
    uint8_t buf[HERMES_NET_DGRAM_MAX + 1];
} hermes_device_udp_t;

typedef struct {
    hermes_device_t device;
    hermes_device_net_t *net;
    signed char srv;   /* the interface's server handle, -1: none */
    uint16_t port;
} hermes_device_tcp_server_t;

typedef hermes_handle_t hermes_device_net_handle_t;
typedef hermes_handle_t hermes_device_tcp_handle_t;
typedef hermes_handle_t hermes_device_udp_handle_t;
typedef hermes_handle_t hermes_device_tcp_server_handle_t;
HERMES_DEVICE_KIND(net);
HERMES_DEVICE_KIND(tcp);
HERMES_DEVICE_KIND(udp);
HERMES_DEVICE_KIND(tcp_server);

static inline void device_net_abort(hermes_device_net_handle_t h) {
    hermes_device_net_t *n = HERMES_DEVICE_GET(hermes_device_net_t, net, h, "abort");
    hermes_net_lock();
    if (n->ops->down) n->ops->down(n->nif);
    hermes_net_unlock();
}

static inline void device_net_describe(hermes_device_net_handle_t h, hermes_device_desc_t *o) {
    hermes_device_net_t *n = HERMES_DEVICE_GET(hermes_device_net_t, net, h, "describe");
    o->bytes = sizeof(hermes_device_net_t);
    HERMES_DEVICE_TEXT(o, "chip", n->what); (void)n;
}

HERMES_DEVICE_CLASS(net, hermes_device_net_handle_t, 0);

/* For a module's constructor: registers an interface and returns it. */
static inline hermes_device_net_handle_t hermes_net_new(const hermes_net_ops_t *ops, void *nif, const char *what) {
    hermes_lock();
    hermes_device_net_t *n = hermes_runtime_alloc(sizeof(hermes_device_net_t));
    if (!n) {
        hermes_unlock();
        HERMES_DIAG("hermes_net: out of memory (%u bytes)\n", (unsigned)sizeof(hermes_device_net_t));
        hermes_fatal();
    }
    n->ops = ops;
    n->nif = nif;
    n->what = what;
    hermes_device_register_or_abort(&n->device, &hermes_device_class_net);
    hermes_unlock();
    return hermes_device_handle(&n->device);
}

#if !HERMES_BARE_METAL
/* The host's own sockets as an interface (hermes_net_host.c). */
extern const hermes_net_ops_t hermes_net_host_ops;
static inline hermes_device_net_handle_t net_host(void) {
    return hermes_net_new(&hermes_net_host_ops, 0, "host");
}
#endif

static inline void hermes_net_bad_ip(const char *what_P, const char *ip) {
    HERMES_DIAG("hermes_net: %" HERMES_PS "(\"%s\"): not an address a.b.c.d\n", what_P, ip);
    (void)what_P; (void)ip;
    hermes_fatal();
}

/* hermes_net.c: an address from "a.b.c.d" or a name (DNS); 0 if neither */
int hermes_net_address_of(hermes_device_net_t *n, const char *text, uint8_t ip[4]);
const char *hermes_net_text_buffer(void); /* one buffer for the methods that return an address */
bool hermes_net_link(hermes_device_net_t *n);
const char *hermes_net_own_ip(hermes_device_net_t *n);
const char *hermes_net_resolve_text(hermes_device_net_t *n, const char *name);
bool hermes_net_set_dns(hermes_device_net_t *n, const char *ip);
bool hermes_net_dhcp(hermes_device_net_t *n);

static inline bool device_net_dhcp(hermes_device_net_handle_t h) {
    return hermes_net_dhcp(HERMES_DEVICE_GET(hermes_device_net_t, net, h, "dhcp"));
}

static inline bool device_net_link(hermes_device_net_handle_t h) {
    return hermes_net_link(HERMES_DEVICE_GET(hermes_device_net_t, net, h, "link"));
}
static inline const char *device_net_ip(hermes_device_net_handle_t h) {
    return hermes_net_own_ip(HERMES_DEVICE_GET(hermes_device_net_t, net, h, "ip"));
}
static inline const char *device_net_resolve(hermes_device_net_handle_t h, const char *name) {
    return hermes_net_resolve_text(HERMES_DEVICE_GET(hermes_device_net_t, net, h, "resolve"), name);
}
static inline bool device_net_dns(hermes_device_net_handle_t h, const char *ip) {
    return hermes_net_set_dns(HERMES_DEVICE_GET(hermes_device_net_t, net, h, "dns"), ip);
}

/* --- TCP ------------------------------------------------------------------------- */

static inline void device_tcp_abort(hermes_device_tcp_handle_t h) {
    hermes_device_tcp_t *c = HERMES_DEVICE_GET(hermes_device_tcp_t, tcp, h, "abort");
    hermes_net_lock();
    if (c->sock >= 0) c->net->ops->close(c->net->nif, c->sock);
    c->sock = -1;
    hermes_net_unlock();
}

static inline void device_tcp_describe(hermes_device_tcp_handle_t h, hermes_device_desc_t *o) {
    hermes_device_tcp_t *c = HERMES_DEVICE_GET(hermes_device_tcp_t, tcp, h, "describe");
    o->bytes = sizeof(hermes_device_tcp_t);
    HERMES_DEVICE_VALUE(o, "port", c->port); (void)c;
}

HERMES_DEVICE_CLASS(tcp, hermes_device_tcp_handle_t, 0);

int hermes_net_tcp_connect(hermes_device_net_t *n, const uint8_t ip[4], uint16_t port);
bool hermes_net_tcp_connected(hermes_device_net_t *n, int sock);
bool hermes_net_tcp_send(hermes_device_net_t *n, int sock, const uint8_t *b, int len);
long hermes_net_tcp_available(hermes_device_net_t *n, int sock);
long hermes_net_tcp_read(hermes_device_net_t *n, int sock, long wait_ms);

static inline hermes_device_tcp_handle_t device_net_tcp(hermes_device_net_handle_t h, const char *ip, long port) {
    hermes_device_net_t *n = HERMES_DEVICE_GET(hermes_device_net_t, net, h, "tcp");
    hermes_device_tcp_t *c;
    uint8_t a[4];
    int known = hermes_net_address_of(n, ip, a); /* a name that DNS doesn't know: not connected */
    hermes_lock();
    c = hermes_runtime_alloc(sizeof(hermes_device_tcp_t));
    if (!c) {
        hermes_unlock();
        HERMES_DIAG("hermes_net: tcp(): out of memory\n");
        hermes_fatal();
    }
    c->net = n;
    c->sock = -1;
    for (int i = 0; i < 4; i++) c->ip[i] = a[i];
    c->port = (uint16_t)port;
    hermes_device_register_or_abort(&c->device, &hermes_device_class_tcp);
    hermes_unlock();
    c->sock = known ? (signed char)hermes_net_tcp_connect(n, a, (uint16_t)port) : -1; /* -1: failed */
    return hermes_device_handle(&c->device);
}

static inline bool device_tcp_connected(hermes_device_tcp_handle_t h) {
    hermes_device_tcp_t *c = HERMES_DEVICE_GET(hermes_device_tcp_t, tcp, h, "connected");
    return c->sock >= 0 && hermes_net_tcp_connected(c->net, c->sock);
}

static inline bool device_tcp_send_text(hermes_device_tcp_handle_t h, const char *s) {
    hermes_device_tcp_t *c = HERMES_DEVICE_GET(hermes_device_tcp_t, tcp, h, "send_text");
    int n = 0;
    while (s[n]) n++;
    return c->sock >= 0 && hermes_net_tcp_send(c->net, c->sock, (const uint8_t *)s, n);
}

static inline bool device_tcp_send_byte(hermes_device_tcp_handle_t h, long b) {
    hermes_device_tcp_t *c = HERMES_DEVICE_GET(hermes_device_tcp_t, tcp, h, "send_byte");
    uint8_t x = (uint8_t)b;
    return c->sock >= 0 && hermes_net_tcp_send(c->net, c->sock, &x, 1);
}

static inline long device_tcp_available(hermes_device_tcp_handle_t h) {
    hermes_device_tcp_t *c = HERMES_DEVICE_GET(hermes_device_tcp_t, tcp, h, "available");
    return c->sock >= 0 ? hermes_net_tcp_available(c->net, c->sock) : 0;
}

static inline long device_tcp_read(hermes_device_tcp_handle_t h) {
    hermes_device_tcp_t *c = HERMES_DEVICE_GET(hermes_device_tcp_t, tcp, h, "read");
    return c->sock >= 0 ? hermes_net_tcp_read(c->net, c->sock, 0) : -1;
}

static inline long device_tcp_read_wait(hermes_device_tcp_handle_t h, long ms) {
    hermes_device_tcp_t *c = HERMES_DEVICE_GET(hermes_device_tcp_t, tcp, h, "read_wait");
    return c->sock >= 0 ? hermes_net_tcp_read(c->net, c->sock, ms) : -1;
}

static inline void hermes_tcp_release(hermes_device_t *d) {
    device_tcp_abort(hermes_device_handle(d));
}

static inline void device_tcp_destroy(hermes_device_tcp_handle_t h) {
    hermes_device_tcp_t *c = HERMES_DEVICE_GET(hermes_device_tcp_t, tcp, h, "destroy");
    device_tcp_abort(h); /* closed outside the framework's lock: a chip's answer may take a while */
    hermes_device_destroy(&c->device, hermes_tcp_release);
}

/* --- UDP ------------------------------------------------------------------------- */

static inline void device_udp_abort(hermes_device_udp_handle_t h) {
    hermes_device_udp_t *u = HERMES_DEVICE_GET(hermes_device_udp_t, udp, h, "abort");
    hermes_net_lock();
    if (u->sock >= 0) u->net->ops->close(u->net->nif, u->sock);
    u->sock = -1;
    hermes_net_unlock();
}

static inline void device_udp_describe(hermes_device_udp_handle_t h, hermes_device_desc_t *o) {
    hermes_device_udp_t *u = HERMES_DEVICE_GET(hermes_device_udp_t, udp, h, "describe");
    o->bytes = sizeof(hermes_device_udp_t);
    HERMES_DEVICE_VALUE(o, "port", u->port); (void)u;
}

HERMES_DEVICE_CLASS(udp, hermes_device_udp_handle_t, 0);

bool hermes_net_udp_send(hermes_device_net_t *n, int sock, const uint8_t ip[4], uint16_t port, const uint8_t *b, int len);
int hermes_net_udp_receive(hermes_device_net_t *n, int sock, uint8_t *buf, int max, uint8_t from[4],
                           uint16_t *from_port, long wait_ms);

static inline hermes_device_udp_handle_t device_net_udp(hermes_device_net_handle_t h, long port) {
    hermes_device_net_t *n = HERMES_DEVICE_GET(hermes_device_net_t, net, h, "udp");
    hermes_lock();
    hermes_device_udp_t *u = hermes_runtime_alloc(sizeof(hermes_device_udp_t));
    if (!u) {
        hermes_unlock();
        HERMES_DIAG("hermes_net: udp(%ld): out of memory (%u bytes)\n", port, (unsigned)sizeof(hermes_device_udp_t));
        hermes_fatal();
    }
    u->net = n;
    u->port = (uint16_t)port;
    u->len = -1;
    hermes_device_register_or_abort(&u->device, &hermes_device_class_udp);
    hermes_unlock();
    hermes_net_lock();
    u->sock = (signed char)n->ops->udp_open(n->nif, (uint16_t)port);
    hermes_net_unlock();
    return hermes_device_handle(&u->device);
}

static inline bool device_udp_send_to(hermes_device_udp_handle_t h, const char *ip, long port, const char *s) {
    hermes_device_udp_t *u = HERMES_DEVICE_GET(hermes_device_udp_t, udp, h, "send_to");
    uint8_t a[4];
    if (u->sock < 0 || !hermes_net_address_of(u->net, ip, a)) return false;
    int n = 0;
    while (s[n]) n++;
    return u->sock >= 0 && hermes_net_udp_send(u->net, u->sock, a, (uint16_t)port, (const uint8_t *)s, n);
}

static inline long device_udp_receive_wait(hermes_device_udp_handle_t h, long ms) {
    hermes_device_udp_t *u = HERMES_DEVICE_GET(hermes_device_udp_t, udp, h, "receive_wait");
    if (u->sock < 0) return -1;
    u->len = hermes_net_udp_receive(u->net, u->sock, u->buf, HERMES_NET_DGRAM_MAX, u->from, &u->from_port, ms);
    return u->len;
}

static inline long device_udp_receive(hermes_device_udp_handle_t h) {
    return device_udp_receive_wait(h, 0);
}

static inline long device_udp_get(hermes_device_udp_handle_t h, long i) {
    hermes_device_udp_t *u = HERMES_DEVICE_GET(hermes_device_udp_t, udp, h, "get");
    return (i >= 0 && i < u->len) ? u->buf[i] : -1;
}

static inline const char *device_udp_text(hermes_device_udp_handle_t h) {
    hermes_device_udp_t *u = HERMES_DEVICE_GET(hermes_device_udp_t, udp, h, "text");
    if (u->len < 0) return "";
    u->buf[u->len] = 0;
    return (const char *)u->buf;
}

static inline const char *device_udp_from_ip(hermes_device_udp_handle_t h) {
    hermes_device_udp_t *u = HERMES_DEVICE_GET(hermes_device_udp_t, udp, h, "from_ip");
    return u->len < 0 ? "" : hermes_net_ip_text(u->from, (char *)hermes_net_text_buffer());
}

static inline long device_udp_from_port(hermes_device_udp_handle_t h) {
    hermes_device_udp_t *u = HERMES_DEVICE_GET(hermes_device_udp_t, udp, h, "from_port");
    return u->len < 0 ? -1L : (long)u->from_port;
}

static inline void hermes_udp_release(hermes_device_t *d) {
    device_udp_abort(hermes_device_handle(d));
}

static inline void device_udp_destroy(hermes_device_udp_handle_t h) {
    hermes_device_udp_t *u = HERMES_DEVICE_GET(hermes_device_udp_t, udp, h, "destroy");
    device_udp_abort(h); /* closed outside the framework's lock (see device_tcp_destroy) */
    hermes_device_destroy(&u->device, hermes_udp_release);
}

/* --- TCP server ------------------------------------------------------------------ */

static inline void device_tcp_server_abort(hermes_device_tcp_server_handle_t h) {
    hermes_device_tcp_server_t *s = HERMES_DEVICE_GET(hermes_device_tcp_server_t, tcp_server, h, "abort");
    hermes_net_lock();
    if (s->srv >= 0 && s->net->ops->unlisten) s->net->ops->unlisten(s->net->nif, s->srv);
    s->srv = -1;
    hermes_net_unlock();
}

static inline void device_tcp_server_describe(hermes_device_tcp_server_handle_t h, hermes_device_desc_t *o) {
    hermes_device_tcp_server_t *s = HERMES_DEVICE_GET(hermes_device_tcp_server_t, tcp_server, h, "describe");
    o->bytes = sizeof(hermes_device_tcp_server_t);
    HERMES_DEVICE_VALUE(o, "port", s->port); (void)s;
}

HERMES_DEVICE_CLASS(tcp_server, hermes_device_tcp_server_handle_t, 0);

/* An interface without servers (or with all its sockets taken): the
 * device exists and never has a client waiting. */
static inline hermes_device_tcp_server_handle_t device_net_listen(hermes_device_net_handle_t h, long port) {
    hermes_device_net_t *n = HERMES_DEVICE_GET(hermes_device_net_t, net, h, "listen");
    hermes_lock();
    hermes_device_tcp_server_t *s = hermes_runtime_alloc(sizeof(hermes_device_tcp_server_t));
    if (!s) {
        hermes_unlock();
        HERMES_DIAG("hermes_net: listen(%ld): out of memory\n", port);
        hermes_fatal();
    }
    s->net = n;
    s->port = (uint16_t)port;
    hermes_device_register_or_abort(&s->device, &hermes_device_class_tcp_server);
    hermes_unlock();
    hermes_net_lock();
    s->srv = (signed char)(n->ops->listen ? n->ops->listen(n->nif, (uint16_t)port) : -1);
    hermes_net_unlock();
    return hermes_device_handle(&s->device);
}

static inline bool device_tcp_server_waiting(hermes_device_tcp_server_handle_t h) {
    hermes_device_tcp_server_t *s = HERMES_DEVICE_GET(hermes_device_tcp_server_t, tcp_server, h, "waiting");
    if (s->srv < 0) return false;
    hermes_net_lock();
    int sock = s->net->ops->accept(s->net->nif, s->srv, 0);
    hermes_net_unlock();
    return sock >= 0;
}

static inline hermes_device_tcp_handle_t device_tcp_server_accept(hermes_device_tcp_server_handle_t h) {
    hermes_device_tcp_server_t *s = HERMES_DEVICE_GET(hermes_device_tcp_server_t, tcp_server, h, "accept");
    hermes_lock();
    hermes_device_tcp_t *c = hermes_runtime_alloc(sizeof(hermes_device_tcp_t));
    if (!c) {
        hermes_unlock();
        HERMES_DIAG("hermes_net: accept(): out of memory\n");
        hermes_fatal();
    }
    c->net = s->net;
    c->sock = -1;
    for (int i = 0; i < 4; i++) c->ip[i] = 0;
    c->port = s->port;
    hermes_device_register_or_abort(&c->device, &hermes_device_class_tcp);
    hermes_unlock();
    if (s->srv >= 0) {
        hermes_net_lock();
        c->sock = (signed char)s->net->ops->accept(s->net->nif, s->srv, 1);
        hermes_net_unlock();
    }
    return hermes_device_handle(&c->device);
}

static inline void hermes_tcp_server_release(hermes_device_t *d) {
    device_tcp_server_abort(hermes_device_handle(d));
}

static inline void device_tcp_server_destroy(hermes_device_tcp_server_handle_t h) {
    hermes_device_tcp_server_t *s = HERMES_DEVICE_GET(hermes_device_tcp_server_t, tcp_server, h, "destroy");
    device_tcp_server_abort(h); /* outside the framework's lock (see device_tcp_destroy) */
    hermes_device_destroy(&s->device, hermes_tcp_server_release);
}

#endif
