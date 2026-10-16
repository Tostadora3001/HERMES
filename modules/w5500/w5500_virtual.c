/* A virtual W5500: see w5500_virtual.h. What it models (WIZnet W5500
 * datasheet v1.0.x): the SPI frame (address 2 bytes, control byte = block
 * << 3 | write << 2 | mode, data with the address incrementing); the
 * common registers it needs (MR's reset, VERSIONR = 0x04, PHYCFGR with the
 * link up); and, per socket, Sn_MR, Sn_CR, Sn_IR, Sn_SR, Sn_PORT,
 * Sn_DIPR, Sn_DPORT, the 2 KB TX/RX buffers and their pointers
 * (Sn_TX_FSR/RD/WR, Sn_RX_RSR/RD/WR), with the commands OPEN, CONNECT,
 * LISTEN, SEND, RECV, DISCON and CLOSE. UDP datagrams arrive with the
 * chip's 8-byte header (IP, port, length). Commands complete at once
 * (Sn_CR reads 0).
 *
 * The test network: its sockets are the PC's, and a test can't bind ports
 * below 1024 -- so a privileged port is moved to 47000 + port: a local
 * port (DHCP's 68 -> 47068), and a destination on this PC or broadcast
 * (255.255.255.255:67 -> 127.0.0.1:47067, DNS 127.0.0.1:53 ->
 * 127.0.0.1:47053); answers from 127.0.0.1:47000+p, p a destination so
 * moved, come back from port p.
 * scripts/echo_server.py has a DHCP and a DNS server on those ports. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "w5500_virtual.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NSOCK 8
#define BUF 2048

/* socket registers (offsets in the socket's register block) */
#define SN_MR 0x00
#define SN_CR 0x01
#define SN_IR 0x02
#define SN_SR 0x03
#define SN_PORT 0x04
#define SN_DIPR 0x0C
#define SN_DPORT 0x10
#define SN_TX_FSR 0x20
#define SN_TX_RD 0x22
#define SN_TX_WR 0x24
#define SN_RX_RSR 0x26
#define SN_RX_RD 0x28
#define SN_RX_WR 0x2A

struct w5500v {
    uint8_t common[0x40];
    uint8_t sreg[NSOCK][0x30];
    uint8_t tx[NSOCK][BUF], rx[NSOCK][BUF];
    int fd[NSOCK];
    struct { uint16_t port; int fd; } lst[NSOCK]; /* the PC's listening sockets, by port */
    uint16_t moved_ports[8]; /* destinations moved (see the top): their answers come back from them */
    /* the frame being transferred */
    int idx, write;
    uint16_t addr;
    uint8_t block;
};

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void sock_close(w5500v_t *w, int s) {
    if (w->fd[s] >= 0) close(w->fd[s]);
    w->fd[s] = -1;
    w->sreg[s][SN_SR] = 0x00;
}

/* the test network's port moves (see above) */
static uint16_t moved(uint16_t port) { return port && port < 1024 ? (uint16_t)(47000 + port) : port; }
static void move_dest(w5500v_t *w, struct sockaddr_in *a) {
    const uint8_t *ip = (const uint8_t *)&a->sin_addr.s_addr;
    uint16_t port = ntohs(a->sin_port);
    int local = ip[0] == 127 || (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255);
    if (!local || !port || port >= 1024) return;
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a->sin_port = htons(moved(port));
    for (int i = 0; i < 8; i++) {
        if (w->moved_ports[i] == port) return;
        if (!w->moved_ports[i]) { w->moved_ports[i] = port; return; }
    }
}

/* an answer from 127.0.0.1:47000+p, p a moved destination: from port p */
static uint16_t unmoved(w5500v_t *w, const struct sockaddr_in *a) {
    uint16_t from = ntohs(a->sin_port);
    if (((const uint8_t *)&a->sin_addr.s_addr)[0] != 127 || from <= 47000) return from;
    for (int i = 0; i < 8 && w->moved_ports[i]; i++)
        if (w->moved_ports[i] == from - 47000) return (uint16_t)(from - 47000);
    return from;
}

static int listener(w5500v_t *w, uint16_t port) {
    int free = -1;
    for (int i = 0; i < NSOCK; i++) {
        if (w->lst[i].fd >= 0 && w->lst[i].port == port) return w->lst[i].fd;
        if (w->lst[i].fd < 0 && free < 0) free = i;
    }
    if (free < 0) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(moved(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) { close(fd); return -1; }
    w->lst[free].fd = fd;
    w->lst[free].port = port;
    return fd;
}

static void reset(w5500v_t *w) {
    for (int s = 0; s < NSOCK; s++) if (w->fd[s] >= 0) close(w->fd[s]);
    for (int i = 0; i < NSOCK; i++) {
        if (w->lst[i].fd > 0) close(w->lst[i].fd);
        w->lst[i].fd = -1;
    }
    memset(w->common, 0, sizeof(w->common));
    memset(w->sreg, 0, sizeof(w->sreg));
    for (int s = 0; s < NSOCK; s++) w->fd[s] = -1;
    w->common[0x39] = 0x04; /* VERSIONR */
    w->common[0x2E] = 0x07; /* PHYCFGR: link up, 100 Mb, full duplex */
}

w5500v_t *w5500v_new(void) {
    w5500v_t *w = calloc(1, sizeof(*w));
    for (int s = 0; s < NSOCK; s++) w->fd[s] = -1;
    for (int i = 0; i < NSOCK; i++) w->lst[i].fd = 0; /* 0: never opened (reset closes > 0) */
    reset(w);
    return w;
}

static void addr_of(const uint8_t *r, struct sockaddr_in *a) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    memcpy(&a->sin_addr.s_addr, r + SN_DIPR, 4);
    a->sin_port = htons(get16(r + SN_DPORT));
}

/* Data from the host's sockets into the RX buffers. */
static void poll_rx(w5500v_t *w) {
    for (int s = 0; s < NSOCK; s++) {
        uint8_t *r = w->sreg[s];
        if (r[SN_SR] == 0x14) { /* LISTEN: a client of the PC's listening socket */
            int l = listener(w, get16(r + SN_PORT));
            int fd = l >= 0 ? accept(l, NULL, NULL) : -1;
            if (fd >= 0) { w->fd[s] = fd; r[SN_SR] = 0x17; r[SN_IR] |= 0x01; }
            continue;
        }
        if (w->fd[s] < 0) continue;
        uint16_t wr = get16(r + SN_RX_WR), rd = get16(r + SN_RX_RD);
        int free = BUF - (uint16_t)(wr - rd);
        if (r[SN_SR] == 0x17) { /* TCP established */
            uint8_t tmp[BUF];
            if (free <= 0) continue;
            ssize_t n = recv(w->fd[s], tmp, (size_t)free, MSG_DONTWAIT);
            if (n == 0) { r[SN_SR] = 0x1C; continue; } /* CLOSE_WAIT: the peer closed */
            for (ssize_t i = 0; i < n; i++) w->rx[s][(uint16_t)(wr + i) % BUF] = tmp[i];
            if (n > 0) put16(r + SN_RX_WR, (uint16_t)(wr + n));
        } else if (r[SN_SR] == 0x22) { /* UDP */
            uint8_t tmp[BUF];
            struct sockaddr_in a;
            socklen_t len = sizeof(a);
            ssize_t n = recvfrom(w->fd[s], tmp, sizeof(tmp), MSG_DONTWAIT, (struct sockaddr *)&a, &len);
            if (n < 0 || free < 8 + n) continue; /* none, or no room: dropped (as the chip) */
            uint8_t h[8];
            uint16_t from = unmoved(w, &a);
            memcpy(h, &a.sin_addr.s_addr, 4);
            put16(h + 4, from);
            put16(h + 6, (uint16_t)n);
            for (int i = 0; i < 8; i++) w->rx[s][(uint16_t)(wr + i) % BUF] = h[i];
            for (ssize_t i = 0; i < n; i++) w->rx[s][(uint16_t)(wr + 8 + i) % BUF] = tmp[i];
            put16(r + SN_RX_WR, (uint16_t)(wr + 8 + n));
        }
    }
}

static void command(w5500v_t *w, int s, uint8_t cmd) {
    uint8_t *r = w->sreg[s];
    struct sockaddr_in a;
    switch (cmd) {
    case 0x01: /* OPEN */
        sock_close(w, s);
        put16(r + SN_TX_RD, 0); put16(r + SN_TX_WR, 0);
        put16(r + SN_RX_RD, 0); put16(r + SN_RX_WR, 0);
        if ((r[SN_MR] & 0x0F) == 0x01) {
            r[SN_SR] = 0x13; /* INIT: CONNECT comes next */
        } else if ((r[SN_MR] & 0x0F) == 0x02) {
            w->fd[s] = socket(AF_INET, SOCK_DGRAM, 0);
            int one = 1;
            setsockopt(w->fd[s], SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            setsockopt(w->fd[s], SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
            struct sockaddr_in b;
            memset(&b, 0, sizeof(b));
            b.sin_family = AF_INET;
            b.sin_port = htons(moved(get16(r + SN_PORT))); /* its local port */
            if (bind(w->fd[s], (struct sockaddr *)&b, sizeof(b)) < 0) { sock_close(w, s); break; }
            r[SN_SR] = 0x22;
        }
        break;
    case 0x02: /* LISTEN */
        if (r[SN_SR] == 0x13 && listener(w, get16(r + SN_PORT)) >= 0) r[SN_SR] = 0x14;
        break;
    case 0x04: /* CONNECT */
        if (r[SN_SR] != 0x13) break;
        w->fd[s] = socket(AF_INET, SOCK_STREAM, 0);
        addr_of(r, &a);
        if (connect(w->fd[s], (struct sockaddr *)&a, sizeof(a)) == 0) {
            r[SN_SR] = 0x17;
            r[SN_IR] |= 0x01; /* CON */
        } else {
            sock_close(w, s);
            r[SN_IR] |= 0x08; /* TIMEOUT */
        }
        break;
    case 0x20: { /* SEND: TX_RD .. TX_WR */
        uint16_t rd = get16(r + SN_TX_RD), wr = get16(r + SN_TX_WR);
        uint16_t n = (uint16_t)(wr - rd);
        uint8_t tmp[BUF];
        for (uint16_t i = 0; i < n && i < BUF; i++) tmp[i] = w->tx[s][(uint16_t)(rd + i) % BUF];
        ssize_t sent = -1;
        if (r[SN_SR] == 0x17 && w->fd[s] >= 0) {
            sent = send(w->fd[s], tmp, n, MSG_NOSIGNAL);
        } else if (r[SN_SR] == 0x22 && w->fd[s] >= 0) {
            addr_of(r, &a);
            move_dest(w, &a);
            sent = sendto(w->fd[s], tmp, n, 0, (struct sockaddr *)&a, sizeof(a));
        }
        put16(r + SN_TX_RD, wr);
        r[SN_IR] |= sent == (ssize_t)n ? 0x10 : 0x08; /* SEND_OK, or TIMEOUT as the chip's */
        break;
    }
    case 0x40: /* RECV: RX_RD was moved by the MCU */
        break;
    case 0x08: /* DISCON */
    case 0x10: /* CLOSE */
        sock_close(w, s);
        break;
    }
}

static uint8_t read_reg(w5500v_t *w, uint8_t block, uint16_t addr) {
    if (block == 0) return addr < sizeof(w->common) ? w->common[addr] : 0;
    int s = (block - 1) / 4, kind = (block - 1) % 4;
    if (s >= NSOCK) return 0;
    if (kind == 1) return w->tx[s][addr % BUF];
    if (kind == 2) return w->rx[s][addr % BUF];
    if (kind != 0 || addr >= 0x30) return 0;
    uint8_t *r = w->sreg[s];
    uint16_t v;
    switch (addr) {
    case SN_TX_FSR: case SN_TX_FSR + 1:
        v = (uint16_t)(BUF - (uint16_t)(get16(r + SN_TX_WR) - get16(r + SN_TX_RD)));
        return addr == SN_TX_FSR ? (uint8_t)(v >> 8) : (uint8_t)v;
    case SN_RX_RSR: case SN_RX_RSR + 1:
        v = (uint16_t)(get16(r + SN_RX_WR) - get16(r + SN_RX_RD));
        return addr == SN_RX_RSR ? (uint8_t)(v >> 8) : (uint8_t)v;
    case SN_CR:
        return 0; /* commands complete at once */
    default:
        return r[addr];
    }
}

static void write_reg(w5500v_t *w, uint8_t block, uint16_t addr, uint8_t v) {
    if (block == 0) {
        if (addr == 0x00 && (v & 0x80)) { reset(w); return; } /* MR: software reset */
        if (addr < sizeof(w->common) && addr != 0x39) w->common[addr] = v;
        return;
    }
    int s = (block - 1) / 4, kind = (block - 1) % 4;
    if (s >= NSOCK) return;
    if (kind == 1) { w->tx[s][addr % BUF] = v; return; }
    if (kind == 2) { w->rx[s][addr % BUF] = v; return; }
    if (kind != 0 || addr >= 0x30) return;
    if (addr == SN_CR) { command(w, s, v); return; }
    if (addr == SN_IR) { w->sreg[s][SN_IR] &= (uint8_t)~v; return; } /* write 1 to clear */
    if (addr == SN_SR || addr == SN_TX_FSR || addr == SN_TX_FSR + 1 || addr == SN_RX_RSR || addr == SN_RX_RSR + 1)
        return; /* read only */
    w->sreg[s][addr] = v;
}

void w5500v_select(w5500v_t *w, int selected) {
    w->idx = 0;
    if (selected) poll_rx(w);
}

uint8_t w5500v_transfer(w5500v_t *w, uint8_t in) {
    int i = w->idx++;
    if (i == 0) { w->addr = (uint16_t)(in << 8); return 0x01; }
    if (i == 1) { w->addr |= in; return 0x02; }
    if (i == 2) { w->block = in >> 3; w->write = (in >> 2) & 1; return 0x03; }
    uint8_t out = 0;
    if (w->write) write_reg(w, w->block, w->addr, in);
    else out = read_reg(w, w->block, w->addr);
    w->addr++;
    return out;
}
