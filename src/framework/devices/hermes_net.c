/* TCP and UDP sockets over an interface: the common part (see
 * hermes_net.h). Waits (connecting, a full send, a read with a timeout)
 * sleep 1 ms at a time (delay_ms: under the OS the thread sleeps and others
 * run). One user of the interfaces at a time: a flag taken under the
 * framework lock -- not the lock itself, which on bare metal masks
 * interrupts, and an SPI exchange with a chip takes a while. */
#include "hermes_net.h"
#include "hermes_drv_timer.h"

static volatile unsigned char g_busy;

void hermes_net_lock(void) {
    for (;;) {
        hermes_lock();
        int got = !g_busy;
        if (got) g_busy = 1;
        hermes_unlock();
        if (got) return;
        hermes_thread_yield();
    }
}

void hermes_net_unlock(void) { g_busy = 0; }

bool hermes_net_parse_ip(const char *t, uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        if (*t < '0' || *t > '9') return false;
        unsigned v = 0;
        int digits = 0;
        while (*t >= '0' && *t <= '9') { v = v * 10 + (unsigned)(*t++ - '0'); if (++digits > 3) return false; }
        if (v > 255) return false;
        ip[i] = (uint8_t)v;
        if (i < 3) { if (*t != '.') return false; t++; }
    }
    return *t == 0;
}

const char *hermes_net_ip_text(const uint8_t ip[4], char out[16]) {
    char *p = out;
    for (int i = 0; i < 4; i++) {
        unsigned v = ip[i];
        if (v >= 100) *p++ = (char)('0' + v / 100);
        if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
        *p++ = (char)('0' + v % 10);
        if (i < 3) *p++ = '.';
    }
    *p = 0;
    return out;
}

static int status(hermes_device_net_t *n, int s) {
    hermes_net_lock();
    int st = n->ops->status(n->nif, s);
    hermes_net_unlock();
    return st;
}

int hermes_net_tcp_connect(hermes_device_net_t *n, const uint8_t ip[4], uint16_t port) {
    hermes_net_lock();
    int s = n->ops->tcp_open(n->nif, ip, port);
    hermes_net_unlock();
    if (s < 0) return -1;
    uint32_t start = hermes_timer_millis();
    for (;;) {
        int st = status(n, s);
        if (st == HERMES_NET_CONNECTED) return s;
        if (st == HERMES_NET_CLOSED || hermes_timer_millis() - start >= HERMES_NET_CONNECT_MS) break;
        hermes_timer_delay_ms(1);
    }
    hermes_net_lock();
    n->ops->close(n->nif, s);
    hermes_net_unlock();
    return -1;
}

bool hermes_net_tcp_connected(hermes_device_net_t *n, int sock) {
    return status(n, sock) == HERMES_NET_CONNECTED;
}

/* All of it, or false (closed, or no room for HERMES_NET_CONNECT_MS). */
bool hermes_net_tcp_send(hermes_device_net_t *n, int sock, const uint8_t *b, int len) {
    uint32_t start = hermes_timer_millis();
    while (len > 0) {
        hermes_net_lock();
        int sent = n->ops->send(n->nif, sock, b, len);
        hermes_net_unlock();
        if (sent < 0) return false;
        b += sent;
        len -= sent;
        if (len == 0) break;
        if (hermes_timer_millis() - start >= HERMES_NET_CONNECT_MS) return false;
        hermes_timer_delay_ms(1);
    }
    return true;
}

long hermes_net_tcp_available(hermes_device_net_t *n, int sock) {
    hermes_net_lock();
    int a = n->ops->available(n->nif, sock);
    hermes_net_unlock();
    return a < 0 ? 0 : a;
}

long hermes_net_tcp_read(hermes_device_net_t *n, int sock, long wait_ms) {
    uint32_t start = hermes_timer_millis();
    for (;;) {
        uint8_t b;
        hermes_net_lock();
        int got = n->ops->recv(n->nif, sock, &b, 1);
        hermes_net_unlock();
        if (got == 1) return b;
        if (wait_ms <= 0 || (long)(hermes_timer_millis() - start) >= wait_ms) return -1;
        hermes_timer_delay_ms(1);
    }
}

bool hermes_net_udp_send(hermes_device_net_t *n, int sock, const uint8_t ip[4], uint16_t port, const uint8_t *b, int len) {
    hermes_net_lock();
    int sent = n->ops->send_to(n->nif, sock, ip, port, b, len);
    hermes_net_unlock();
    return sent == len;
}

int hermes_net_udp_receive(hermes_device_net_t *n, int sock, uint8_t *buf, int max, uint8_t from[4],
                           uint16_t *from_port, long wait_ms) {
    uint32_t start = hermes_timer_millis();
    for (;;) {
        hermes_net_lock();
        int got = n->ops->recv_from(n->nif, sock, buf, max, from, from_port);
        hermes_net_unlock();
        if (got >= 0) return got;
        if (wait_ms <= 0 || (long)(hermes_timer_millis() - start) >= wait_ms) return -1;
        hermes_timer_delay_ms(1);
    }
}

/* --- the interface's own state, names ------------------------------------------- */

static char g_text[16];
const char *hermes_net_text_buffer(void) { return g_text; }

int hermes_net_address_of(hermes_device_net_t *n, const char *text, uint8_t ip[4]) {
    if (hermes_net_parse_ip(text, ip)) return 1;
    if (!n->ops->resolve) return 0;
    hermes_net_lock();
    int found = n->ops->resolve(n->nif, text, ip);
    hermes_net_unlock();
    return found;
}

bool hermes_net_link(hermes_device_net_t *n) {
    if (!n->ops->link) return true;
    hermes_net_lock();
    int up = n->ops->link(n->nif);
    hermes_net_unlock();
    return up != 0;
}

const char *hermes_net_own_ip(hermes_device_net_t *n) {
    uint8_t ip[4];
    if (!n->ops->address) return "";
    hermes_net_lock();
    int has = n->ops->address(n->nif, ip);
    hermes_net_unlock();
    return has ? hermes_net_ip_text(ip, g_text) : "";
}

const char *hermes_net_resolve_text(hermes_device_net_t *n, const char *name) {
    uint8_t ip[4];
    return hermes_net_address_of(n, name, ip) ? hermes_net_ip_text(ip, g_text) : "";
}

bool hermes_net_set_dns(hermes_device_net_t *n, const char *text) {
    uint8_t ip[4];
    if (!hermes_net_parse_ip(text, ip)) hermes_net_bad_ip(HERMES_PSTR("dns"), text);
    if (!n->ops->set_dns) return false;
    hermes_net_lock();
    int ok = n->ops->set_dns(n->nif, ip);
    hermes_net_unlock();
    return ok != 0;
}

bool hermes_net_dhcp(hermes_device_net_t *n) {
    hermes_net_lock();
    int ok;
    if (n->ops->dhcp) {
        ok = n->ops->dhcp(n->nif);
    } else { /* the interface (or its module) does it itself: has it an address? */
        uint8_t ip[4] = { 0, 0, 0, 0 };
        ok = n->ops->address && n->ops->address(n->nif, ip);
    }
    hermes_net_unlock();
    return ok != 0;
}
