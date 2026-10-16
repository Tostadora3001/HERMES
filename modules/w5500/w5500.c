/* The W5500 driver: the chip's sockets as a hermes_net interface (see
 * w5500.h). Everything through the SPI driver (frames: address 2 bytes,
 * control = block << 3 | write << 2 | variable length, data). Register
 * map: WIZnet W5500 datasheet. The chip has TCP, UDP and ARP but no DHCP
 * or DNS: those two are here, over a UDP socket, reading the answers in
 * place in the chip's buffer (no copy in RAM). DHCP, DNS and the server
 * are compiled only with their -DHERMES_NET_* flag (hermes_net.h). */
#include "w5500.h"
#include "hermes_drv_spi.h"
#include "hermes_drv_timer.h"

#define W_COMMON 0
#define W_SREG(s) ((uint8_t)((s) * 4 + 1))
#define W_TXBUF(s) ((uint8_t)((s) * 4 + 2))
#define W_RXBUF(s) ((uint8_t)((s) * 4 + 3))

#define MR 0x0000
#define GAR 0x0001
#define SUBR 0x0005
#define SHAR 0x0009
#define SIPR 0x000F
#define PHYCFGR 0x002E
#define VERSIONR 0x0039
#define SN_MR 0x00
#define SN_CR 0x01
#define SN_IR 0x02
#define SN_SR 0x03
#define SN_PORT 0x04
#define SN_DIPR 0x0C
#define SN_DPORT 0x10
#define SN_TX_FSR 0x20
#define SN_TX_WR 0x24
#define SN_RX_RSR 0x26
#define SN_RX_RD 0x28

#define CMD_OPEN 0x01
#define CMD_LISTEN 0x02
#define CMD_CONNECT 0x04
#define CMD_CLOSE 0x10
#define CMD_SEND 0x20
#define CMD_RECV 0x40

#define SR_CLOSED 0x00
#define SR_INIT 0x13
#define SR_LISTEN 0x14
#define SR_ESTABLISHED 0x17
#define SR_CLOSE_WAIT 0x1C
#define SR_UDP 0x22

#define IR_TIMEOUT 0x08
#define IR_SEND_OK 0x10
#define SEND_MS 2000

#define NSOCK 8
#define NSRV 4
#define RECV_EVERY 250 /* RX_RD + RECV only every so many bytes read (as the Arduino library) */

typedef struct {
    int cs;
    uint8_t used;      /* bit s: socket s taken */
    uint16_t next_port;
    uint8_t mac[6];
    uint8_t dns[4];
    /* TCP reads: the chip's RX_RD as this driver has moved it, and how
     * much of that it hasn't told the chip yet (RECV_EVERY) */
    uint16_t rx_rd[NSOCK];
    uint16_t rx_inc[NSOCK];
    /* servers: a socket listening and, once a client connected to it,
     * that socket waiting to be accepted (-1: none) */
    struct { uint16_t port; signed char listen, ready; } srv[NSRV];
    /* DHCP: the lease */
    uint8_t dhcp, dhcp_server[4];
    uint32_t lease_ms, lease_start;
} w5500_t;

static w5500_t g_chip; /* one W5500 per program */

/* weak: on the host, w5500_mock.c puts a virtual chip on that pin */
extern void hermes_w5500_mock_attach(int cs) __attribute__((weak));

/* --- frames ----------------------------------------------------------------------- */
static void frame(w5500_t *w, uint16_t addr, uint8_t block, int write, uint8_t *data, int n) {
    hermes_spi_begin();
    hermes_spi_select(w->cs, 1);
    hermes_spi_transfer((uint8_t)(addr >> 8));
    hermes_spi_transfer((uint8_t)addr);
    hermes_spi_transfer((uint8_t)(block << 3 | (write ? 0x04 : 0)));
    for (int i = 0; i < n; i++) {
        uint8_t r = hermes_spi_transfer(write ? data[i] : 0);
        if (!write) data[i] = r;
    }
    hermes_spi_select(w->cs, 0);
    hermes_spi_end();
}

static void wr8(w5500_t *w, uint8_t block, uint16_t addr, uint8_t v) { frame(w, addr, block, 1, &v, 1); }
static uint8_t rd8(w5500_t *w, uint8_t block, uint16_t addr) { uint8_t v; frame(w, addr, block, 0, &v, 1); return v; }
static void wr16(w5500_t *w, uint8_t block, uint16_t addr, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    frame(w, addr, block, 1, b, 2);
}
/* a 16-bit counter the chip may change between its two bytes: read until
 * two reads agree (as the datasheet asks for Sn_TX_FSR, Sn_RX_RSR) */
static uint16_t rd16(w5500_t *w, uint8_t block, uint16_t addr) {
    uint16_t a, b;
    do {
        uint8_t x[2];
        frame(w, addr, block, 0, x, 2);
        a = (uint16_t)(x[0] << 8 | x[1]);
        frame(w, addr, block, 0, x, 2);
        b = (uint16_t)(x[0] << 8 | x[1]);
    } while (a != b);
    return a;
}

static void cmd(w5500_t *w, int s, uint8_t c) {
    wr8(w, W_SREG(s), SN_CR, c);
    for (int i = 0; i < 1000 && rd8(w, W_SREG(s), SN_CR); i++) {} /* accepted when it reads 0 */
}

/* --- sockets ---------------------------------------------------------------------- */
static int take(w5500_t *w) {
    for (int s = 0; s < NSOCK; s++)
        if (!(w->used & (1u << s))) { w->used |= (uint8_t)(1u << s); return s; }
    return -1;
}

static void give_back(w5500_t *w, int s) {
    cmd(w, s, CMD_CLOSE);
    w->used &= (uint8_t)~(1u << s);
}

static uint16_t local_port(w5500_t *w) {
    uint16_t p = w->next_port++;
    if (w->next_port < 49152) w->next_port = 49152;
    return p;
}

/* a socket of that protocol open on that port, in state `want`; -1 */
static int open_socket(w5500_t *w, uint8_t proto, uint16_t port, uint8_t want) {
    int s = take(w);
    if (s < 0) return -1;
    cmd(w, s, CMD_CLOSE);
    wr8(w, W_SREG(s), SN_IR, 0xFF);
    wr8(w, W_SREG(s), SN_MR, proto);
    wr16(w, W_SREG(s), SN_PORT, port);
    cmd(w, s, CMD_OPEN);
    if (rd8(w, W_SREG(s), SN_SR) != want) { give_back(w, s); return -1; }
    w->rx_rd[s] = rd16(w, W_SREG(s), SN_RX_RD);
    w->rx_inc[s] = 0;
    return s;
}

/* What's at TX_WR is sent; the next SEND must wait for this one to
 * finish (SEND_OK), as the datasheet and the Arduino library do; TIMEOUT:
 * the peer didn't answer (TCP: the chip closed the socket; UDP: no ARP
 * answer). */
static int send_now(w5500_t *w, int s, uint16_t wr) {
    wr16(w, W_SREG(s), SN_TX_WR, wr);
    cmd(w, s, CMD_SEND);
    uint32_t start = hermes_timer_millis();
    for (;;) {
        uint8_t ir = rd8(w, W_SREG(s), SN_IR);
        if (ir & IR_SEND_OK) { wr8(w, W_SREG(s), SN_IR, IR_SEND_OK); return 1; }
        if (ir & IR_TIMEOUT) { wr8(w, W_SREG(s), SN_IR, IR_TIMEOUT); return 0; }
        if (rd8(w, W_SREG(s), SN_SR) == SR_CLOSED || hermes_timer_millis() - start >= SEND_MS) return 0;
    }
}

static int put(w5500_t *w, int s, const uint8_t *b, int n) {
    uint16_t free = rd16(w, W_SREG(s), SN_TX_FSR);
    if (n > (int)free) n = (int)free;
    if (n <= 0) return 0;
    uint16_t ptr = rd16(w, W_SREG(s), SN_TX_WR);
    frame(w, ptr, W_TXBUF(s), 1, (uint8_t *)b, n);
    return send_now(w, s, (uint16_t)(ptr + n)) ? n : -1;
}

static void set_dest(w5500_t *w, int s, const uint8_t ip[4], uint16_t port) {
    uint8_t a[4] = { ip[0], ip[1], ip[2], ip[3] };
    frame(w, SN_DIPR, W_SREG(s), 1, a, 4);
    wr16(w, W_SREG(s), SN_DPORT, port);
}

/* --- a UDP datagram read in place (recv_from; DHCP, DNS) ------------------------------ */
typedef struct { int s; uint16_t base, len; } dgram_t; /* base: its first data byte */

#if defined(HERMES_NET_DHCP) || defined(HERMES_NET_DNS)
static uint8_t dg8(w5500_t *w, const dgram_t *d, uint16_t off) {
    return off < d->len ? rd8(w, W_RXBUF(d->s), (uint16_t)(d->base + off)) : 0;
}
static uint16_t dg16(w5500_t *w, const dgram_t *d, uint16_t off) {
    return (uint16_t)(dg8(w, d, off) << 8 | dg8(w, d, (uint16_t)(off + 1)));
}
static void dg4(w5500_t *w, const dgram_t *d, uint16_t off, uint8_t out[4]) {
    for (int i = 0; i < 4; i++) out[i] = dg8(w, d, (uint16_t)(off + i));
}
#endif

/* waits up to ms for a datagram on s; 1: d describes it (dg_done after) */
static int dg_wait(w5500_t *w, int s, dgram_t *d, uint32_t ms) {
    uint32_t start = hermes_timer_millis();
    while (rd16(w, W_SREG(s), SN_RX_RSR) < 8) {
        if (hermes_timer_millis() - start >= ms) return 0;
        hermes_timer_delay_ms(1);
    }
    uint16_t rd = rd16(w, W_SREG(s), SN_RX_RD);
    uint8_t h[8];
    frame(w, rd, W_RXBUF(s), 0, h, 8);
    d->s = s;
    d->base = (uint16_t)(rd + 8);
    d->len = (uint16_t)(h[6] << 8 | h[7]);
    return 1;
}

static void dg_done(w5500_t *w, const dgram_t *d) {
    wr16(w, W_SREG(d->s), SN_RX_RD, (uint16_t)(d->base + d->len));
    cmd(w, d->s, CMD_RECV);
}

#if defined(HERMES_NET_DHCP) || defined(HERMES_NET_DNS)
/* writing a datagram piece by piece at TX_WR */
static void tx(w5500_t *w, int s, uint16_t *ptr, const void *data, int n) {
    if (n <= 0) return;
    frame(w, *ptr, W_TXBUF(s), 1, (uint8_t *)data, n);
    *ptr = (uint16_t)(*ptr + n);
}
static void tx8(w5500_t *w, int s, uint16_t *ptr, uint8_t v) { tx(w, s, ptr, &v, 1); }
static void tx_zeros(w5500_t *w, int s, uint16_t *ptr, int n) {
    uint8_t z[16] = { 0 };
    while (n > 0) { int k = n > 16 ? 16 : n; tx(w, s, ptr, z, k); n -= k; }
}
#endif

/* --- DHCP (RFC 2131): DISCOVER -> OFFER -> REQUEST -> ACK ---------------------------- */
#ifdef HERMES_NET_DHCP
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_NAK 6

static uint32_t g_xid;

static int dhcp_send(w5500_t *w, int s, uint8_t type, const uint8_t ciaddr[4], const uint8_t want[4], const uint8_t server[4]) {
    static const uint8_t bcast[4] = { 255, 255, 255, 255 };
    set_dest(w, s, bcast, 67);
    uint16_t ptr = rd16(w, W_SREG(s), SN_TX_WR), start = ptr;
    uint8_t head[12] = { 1, 1, 6, 0, (uint8_t)(g_xid >> 24), (uint8_t)(g_xid >> 16), (uint8_t)(g_xid >> 8), (uint8_t)g_xid,
                         0, 0, 0x80, 0 }; /* op, htype, hlen, hops, xid, secs, flags: broadcast */
    tx(w, s, &ptr, head, 12);
    tx(w, s, &ptr, ciaddr, 4);                 /* ciaddr */
    tx_zeros(w, s, &ptr, 12);                  /* yiaddr, siaddr, giaddr */
    tx(w, s, &ptr, w->mac, 6);                 /* chaddr */
    tx_zeros(w, s, &ptr, 10 + 64 + 128);       /* chaddr's rest, sname, file */
    static const uint8_t cookie[4] = { 99, 130, 83, 99 };
    tx(w, s, &ptr, cookie, 4);
    uint8_t t[3] = { 53, 1, type };
    tx(w, s, &ptr, t, 3);
    if (want) { tx8(w, s, &ptr, 50); tx8(w, s, &ptr, 4); tx(w, s, &ptr, want, 4); }
    if (server) { tx8(w, s, &ptr, 54); tx8(w, s, &ptr, 4); tx(w, s, &ptr, server, 4); }
    static const uint8_t params[5] = { 55, 3, 1, 3, 6 }; /* mask, router, DNS */
    tx(w, s, &ptr, params, 5);
    tx8(w, s, &ptr, 255);
    int len = (uint16_t)(ptr - start);
    if (len < 300) tx_zeros(w, s, &ptr, 300 - len); /* BOOTP's minimum: some servers ask for it */
    return send_now(w, s, ptr);
}

/* An answer to our xid: its type; yiaddr, mask, router, DNS, server, lease */
typedef struct { uint8_t type, ip[4], mask[4], router[4], dns[4], server[4]; uint32_t lease_s; } dhcp_answer_t;

static int dhcp_receive(w5500_t *w, int s, dhcp_answer_t *a, uint32_t ms) {
    uint32_t start = hermes_timer_millis();
    for (;;) {
        uint32_t spent = hermes_timer_millis() - start;
        if (spent >= ms) return 0;
        dgram_t d;
        if (!dg_wait(w, s, &d, ms - spent)) return 0;
        uint32_t xid = (uint32_t)dg16(w, &d, 4) << 16 | dg16(w, &d, 6);
        if (dg8(w, &d, 0) != 2 || xid != g_xid || dg16(w, &d, 236) != 0x6382 || dg16(w, &d, 238) != 0x5363) {
            dg_done(w, &d); /* not ours */
            continue;
        }
        a->type = 0;
        a->lease_s = 0;
        dg4(w, &d, 16, a->ip);
        for (uint16_t o = 240; o < d.len;) {
            uint8_t code = dg8(w, &d, o);
            if (code == 255) break;
            if (code == 0) { o++; continue; }
            uint8_t l = dg8(w, &d, (uint16_t)(o + 1));
            uint16_t v = (uint16_t)(o + 2);
            switch (code) {
            case 53: a->type = dg8(w, &d, v); break;
            case 1: dg4(w, &d, v, a->mask); break;
            case 3: dg4(w, &d, v, a->router); break;
            case 6: dg4(w, &d, v, a->dns); break;
            case 54: dg4(w, &d, v, a->server); break;
            case 51: a->lease_s = (uint32_t)dg16(w, &d, v) << 16 | dg16(w, &d, (uint16_t)(v + 2)); break;
            }
            o = (uint16_t)(v + l);
        }
        dg_done(w, &d);
        return 1;
    }
}

static void apply(w5500_t *w, const dhcp_answer_t *a) {
    uint8_t ip[4], mask[4], gw[4];
    for (int i = 0; i < 4; i++) { ip[i] = a->ip[i]; mask[i] = a->mask[i]; gw[i] = a->router[i]; }
    frame(w, SIPR, W_COMMON, 1, ip, 4);
    frame(w, SUBR, W_COMMON, 1, mask, 4);
    frame(w, GAR, W_COMMON, 1, gw, 4);
    if (a->dns[0]) for (int i = 0; i < 4; i++) w->dns[i] = a->dns[i];
    for (int i = 0; i < 4; i++) w->dhcp_server[i] = a->server[i];
    /* a lease longer than ~24 days is renewed then (32-bit ms) */
    w->lease_ms = a->lease_s > 2000000UL ? 2000000000UL : a->lease_s * 1000UL;
    w->lease_start = hermes_timer_millis();
}

/* renew = 1: REQUEST for the address we have, straight to the server.
 * 1: an address (or the same one again). Three tries, 2 s each. */
static int dhcp_run(w5500_t *w, int renew) {
    int s = open_socket(w, 0x02, 68, SR_UDP);
    if (s < 0) return 0;
    int ok = 0;
    uint8_t zero[4] = { 0, 0, 0, 0 }, mine[4];
    frame(w, SIPR, W_COMMON, 0, mine, 4);
    for (int tries = 0; tries < 3 && !ok; tries++) {
        dhcp_answer_t a = { 0 };
        g_xid = (g_xid * 1103515245UL + 12345UL) ^ hermes_timer_millis();
        if (renew) {
            if (!dhcp_send(w, s, DHCP_REQUEST, mine, 0, 0)) continue;
        } else {
            if (!dhcp_send(w, s, DHCP_DISCOVER, zero, 0, 0)) continue;
            if (!dhcp_receive(w, s, &a, 2000) || a.type != DHCP_OFFER) continue;
            uint8_t want[4], server[4];
            for (int i = 0; i < 4; i++) { want[i] = a.ip[i]; server[i] = a.server[i]; }
            if (!dhcp_send(w, s, DHCP_REQUEST, zero, want, server)) continue;
        }
        if (!dhcp_receive(w, s, &a, 2000)) continue;
        if (a.type == DHCP_ACK) { apply(w, &a); ok = 1; }
        else if (a.type == DHCP_NAK && renew) renew = 0; /* the address is gone: start again */
    }
    give_back(w, s);
    return ok;
}

/* the lease is renewed when half of it has passed (checked when a socket
 * opens, a server listens or link() is asked); failing that, a new one */
static void maintain(w5500_t *w) {
    if (!w->dhcp || hermes_timer_millis() - w->lease_start < w->lease_ms / 2) return;
    if (!dhcp_run(w, 1) && !dhcp_run(w, 0)) w->lease_start = hermes_timer_millis(); /* try again later */
}

static int dhcp_(void *nif) {
    w5500_t *w = nif;
    w->dhcp = 1;
    return dhcp_run(w, 0);
}
#define W_DHCP dhcp_
#else
#define maintain(w) ((void)0)
#define W_DHCP 0
#endif

/* --- start -------------------------------------------------------------------------- */
void *hermes_w5500_init(int cs, const uint8_t ip[4]) {
    w5500_t *w = &g_chip;
    w->cs = cs;
    w->used = 0;
    w->next_port = 49152;
    w->dhcp = 0;
    for (int i = 0; i < NSRV; i++) w->srv[i].listen = w->srv[i].ready = -1;
    if (hermes_w5500_mock_attach) hermes_w5500_mock_attach(cs);
    hermes_spi_cs_setup(cs);
    /* A W5500 just powered up (the board's reset) may take a while to
     * answer: the Arduino Ethernet library waits 560 ms before trying.
     * Here: VERSIONR asked again until it reads 4, up to 600 ms. */
    uint32_t start = hermes_timer_millis();
    while (rd8(w, W_COMMON, VERSIONR) != 0x04) {
        if (hermes_timer_millis() - start >= 600) return 0;
        hermes_timer_delay_ms(1);
    }
    /* software reset: done when MR reads 0 again (up to 20 ms, as in the
     * Arduino library) -- clears sockets left open by a previous run */
    wr8(w, W_COMMON, MR, 0x80);
    for (int i = 0; rd8(w, W_COMMON, MR); i++) {
        if (i >= 20) return 0;
        hermes_timer_delay_ms(1);
    }
#ifdef HERMES_NET_DHCP
    g_xid = start;
#endif
    /* MAC: locally administered; with no address yet (DHCP to come), its
     * last bytes from the clock at start, so that two boards don't share one */
    int none = !ip[0] && !ip[1] && !ip[2] && !ip[3];
    uint8_t mac[6] = { 0x02, 0, 0, 0, 0, ip[3] };
    if (none) { mac[3] = (uint8_t)(start >> 16); mac[4] = (uint8_t)(start >> 8); mac[5] = (uint8_t)start; }
    for (int i = 0; i < 6; i++) w->mac[i] = mac[i];
    frame(w, SHAR, W_COMMON, 1, mac, 6);
    uint8_t gw[4] = { ip[0], ip[1], ip[2], none ? 0 : 1 }, mask[4] = { 255, 255, 255, 0 }, me[4];
    for (int i = 0; i < 4; i++) { me[i] = ip[i]; w->dns[i] = gw[i]; } /* DNS: the gateway, unless dns() */
    frame(w, GAR, W_COMMON, 1, gw, 4);
    frame(w, SUBR, W_COMMON, 1, mask, 4);
    frame(w, SIPR, W_COMMON, 1, me, 4);
    return w;
}

/* --- the interface -------------------------------------------------------------------- */
static int tcp_open(void *nif, const uint8_t ip[4], uint16_t port) {
    w5500_t *w = nif;
    maintain(w);
    int s = open_socket(w, 0x01, local_port(w), SR_INIT);
    if (s < 0) return -1;
    set_dest(w, s, ip, port);
    cmd(w, s, CMD_CONNECT);
    return s;
}

static int udp_open(void *nif, uint16_t port) {
    w5500_t *w = nif;
    maintain(w);
    return open_socket(w, 0x02, port, SR_UDP);
}

static int available(void *nif, int s) {
    w5500_t *w = nif;
    return rd16(w, W_SREG(s), SN_RX_RSR) - w->rx_inc[s];
}

static int status(void *nif, int s) {
    w5500_t *w = nif;
    uint8_t sr = rd8(w, W_SREG(s), SN_SR);
    switch (sr) {
    case SR_ESTABLISHED: return HERMES_NET_CONNECTED;
    case SR_INIT: case 0x15: return HERMES_NET_CONNECTING; /* INIT, SYNSENT */
    case SR_UDP: return HERMES_NET_OPEN_UDP;
    case SR_CLOSE_WAIT: /* the peer closed; what it sent can still be read */
        return available(nif, s) > 0 ? HERMES_NET_CONNECTED : HERMES_NET_CLOSED;
    default: return HERMES_NET_CLOSED;
    }
}

static int send_(void *nif, int s, const uint8_t *b, int n) {
    w5500_t *w = nif;
    uint8_t sr = rd8(w, W_SREG(s), SN_SR);
    if (sr != SR_ESTABLISHED && sr != SR_CLOSE_WAIT) return -1; /* the peer may have closed only its half */
    return put(w, s, b, n);
}

static int send_to(void *nif, int s, const uint8_t ip[4], uint16_t port, const uint8_t *b, int n) {
    w5500_t *w = nif;
    set_dest(w, s, ip, port);
    return put(w, s, b, n) == n ? n : -1;
}

/* RX_RD and RECV only every RECV_EVERY bytes or when all is read: fewer
 * SPI frames for a program that reads byte by byte */
static int recv_(void *nif, int s, uint8_t *b, int max) {
    w5500_t *w = nif;
    uint16_t rsr = rd16(w, W_SREG(s), SN_RX_RSR);
    int n = rsr - w->rx_inc[s];
    if (n > max) n = max;
    if (n <= 0) return 0;
    frame(w, w->rx_rd[s], W_RXBUF(s), 0, b, n);
    w->rx_rd[s] = (uint16_t)(w->rx_rd[s] + n);
    w->rx_inc[s] = (uint16_t)(w->rx_inc[s] + n);
    if (w->rx_inc[s] >= RECV_EVERY || w->rx_inc[s] == rsr) {
        wr16(w, W_SREG(s), SN_RX_RD, w->rx_rd[s]);
        cmd(w, s, CMD_RECV);
        w->rx_inc[s] = 0;
    }
    return n;
}

static int recv_from(void *nif, int s, uint8_t *b, int max, uint8_t ip[4], uint16_t *port) {
    w5500_t *w = nif;
    dgram_t d;
    if (!dg_wait(w, s, &d, 0)) return -1; /* none waiting */
    uint8_t h[8];
    frame(w, (uint16_t)(d.base - 8), W_RXBUF(s), 0, h, 8);
    int n = (int)d.len > max ? max : (int)d.len;
    if (n > 0) frame(w, d.base, W_RXBUF(s), 0, b, n);
    for (int i = 0; i < 4; i++) ip[i] = h[i];
    *port = (uint16_t)(h[4] << 8 | h[5]);
    dg_done(w, &d); /* a longer datagram's rest is skipped */
    return n;
}

static void close_(void *nif, int s) {
    give_back(nif, s);
}

#ifdef HERMES_NET_SERVER
/* --- servers: one socket always listening; a client connects to it, and
 *     another socket takes its place ----------------------------------------------- */
static int relisten(w5500_t *w, int i) {
    int s = open_socket(w, 0x01, w->srv[i].port, SR_INIT);
    if (s >= 0) {
        cmd(w, s, CMD_LISTEN);
        if (rd8(w, W_SREG(s), SN_SR) != SR_LISTEN) { give_back(w, s); s = -1; }
    }
    w->srv[i].listen = (signed char)s;
    return s;
}

static int listen_(void *nif, uint16_t port) {
    w5500_t *w = nif;
    maintain(w);
    for (int i = 0; i < NSRV; i++) {
        if (w->srv[i].port) continue;
        w->srv[i].port = port;
        if (relisten(w, i) < 0) { w->srv[i].port = 0; return -1; }
        return i;
    }
    return -1;
}

static int accept_(void *nif, int i, int take) {
    w5500_t *w = nif;
    if (w->srv[i].ready < 0) {
        int s = w->srv[i].listen;
        uint8_t sr = s >= 0 ? rd8(w, W_SREG(s), SN_SR) : SR_CLOSED;
        if (sr == SR_ESTABLISHED || sr == SR_CLOSE_WAIT) {
            w->srv[i].ready = (signed char)s;
            w->rx_rd[s] = rd16(w, W_SREG(s), SN_RX_RD);
            w->rx_inc[s] = 0;
            relisten(w, i);                    /* no socket free: tried again next time */
        } else if (sr != SR_LISTEN && sr != 0x16) { /* 0x16 SYNRECV: a client on its way */
            if (s >= 0) give_back(w, s);
            relisten(w, i);
        }
    }
    int c = w->srv[i].ready;
    if (c >= 0 && take) w->srv[i].ready = -1;
    return c;
}

static void unlisten(void *nif, int i) {
    w5500_t *w = nif;
    if (w->srv[i].listen >= 0) give_back(w, w->srv[i].listen);
    if (w->srv[i].ready >= 0) give_back(w, w->srv[i].ready);
    w->srv[i].listen = w->srv[i].ready = -1;
    w->srv[i].port = 0;
}
#define W_SERVER listen_, accept_, unlisten
#else
#define W_SERVER 0, 0, 0
#endif

static void down(void *nif) {
    w5500_t *w = nif;
    for (int s = 0; s < NSOCK; s++) give_back(w, s);
    for (int i = 0; i < NSRV; i++) { w->srv[i].listen = w->srv[i].ready = -1; w->srv[i].port = 0; }
}

/* --- link, address, DNS ------------------------------------------------------------- */
static int link_(void *nif) {
    w5500_t *w = nif;
    maintain(w);
    return rd8(w, W_COMMON, PHYCFGR) & 0x01; /* LNK: a cable to a live port */
}

static int address(void *nif, uint8_t ip[4]) {
    frame(nif, SIPR, W_COMMON, 0, ip, 4);
    return ip[0] || ip[1] || ip[2] || ip[3];
}

#ifdef HERMES_NET_DNS
static int set_dns(void *nif, const uint8_t ip[4]) {
    w5500_t *w = nif;
    for (int i = 0; i < 4; i++) w->dns[i] = ip[i];
    return 1;
}

/* past a name in a DNS message (labels, ending in 0 or a pointer) */
static uint16_t skip_name(w5500_t *w, const dgram_t *d, uint16_t o) {
    while (o < d->len) {
        uint8_t c = dg8(w, d, o);
        if (c == 0) return (uint16_t)(o + 1);
        if ((c & 0xC0) == 0xC0) return (uint16_t)(o + 2);
        o = (uint16_t)(o + c + 1);
    }
    return o;
}

/* DNS (RFC 1035): one A query to the DNS server, two tries of 2 s */
static int resolve(void *nif, const char *name, uint8_t ip[4]) {
    w5500_t *w = nif;
    if (!name[0]) return 0;
    int s = open_socket(w, 0x02, local_port(w), SR_UDP);
    if (s < 0) return 0;
    int found = 0;
    for (int tries = 0; tries < 2 && !found; tries++) {
        uint16_t id = (uint16_t)(hermes_timer_millis() ^ 0x4845);
        set_dest(w, s, w->dns, 53);
        uint16_t ptr = rd16(w, W_SREG(s), SN_TX_WR);
        uint8_t head[12] = { (uint8_t)(id >> 8), (uint8_t)id, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 }; /* RD, 1 question */
        tx(w, s, &ptr, head, 12);
        for (const char *l = name; *l;) { /* "a.bc" -> 1 a 2 b c 0 */
            int n = 0;
            while (l[n] && l[n] != '.') n++;
            tx8(w, s, &ptr, (uint8_t)n);
            tx(w, s, &ptr, l, n);
            l += n;
            if (*l == '.') l++;
        }
        static const uint8_t tail[5] = { 0, 0, 1, 0, 1 }; /* end of name, type A, class IN */
        tx(w, s, &ptr, tail, 5);
        if (!send_now(w, s, ptr)) continue;
        uint32_t start = hermes_timer_millis();
        dgram_t d;
        for (;;) {
            uint32_t spent = hermes_timer_millis() - start;
            if (found || spent >= 2000 || !dg_wait(w, s, &d, 2000 - spent)) break;
            if (dg16(w, &d, 0) == id && (dg8(w, &d, 3) & 0x0F) == 0) {
                uint16_t answers = dg16(w, &d, 6);
                uint16_t o = (uint16_t)(skip_name(w, &d, 12) + 4); /* past the question */
                for (uint16_t a = 0; a < answers && !found && o < d.len; a++) {
                    o = skip_name(w, &d, o);
                    uint16_t type = dg16(w, &d, o), rdlen = dg16(w, &d, (uint16_t)(o + 8));
                    if (type == 1 && rdlen == 4) { dg4(w, &d, (uint16_t)(o + 10), ip); found = 1; }
                    o = (uint16_t)(o + 10 + rdlen);
                }
                dg_done(w, &d);
                if (!found) { tries = 2; break; } /* an answer: the name has no address */
            } else {
                dg_done(w, &d); /* not ours */
            }
        }
    }
    give_back(w, s);
    return found;
}
#define W_DNS resolve, set_dns
#else
#define W_DNS 0, 0
#endif

const hermes_net_ops_t hermes_w5500_ops = {
    tcp_open, udp_open, status, send_, send_to, available, recv_, recv_from, close_, down,
    W_SERVER, link_, address, W_DNS, W_DHCP,
};
