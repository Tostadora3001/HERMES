/* The host's own sockets as a hermes_net interface (net_host(); see
 * hermes_net.h). Non-blocking POSIX sockets; hermes_net.c does the
 * waiting. Up to 16 sockets. */
#include "hermes_platform.h"
#if !HERMES_BARE_METAL /* the file is empty on a board */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "hermes_net.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define HOST_SOCKETS 16
static struct { int fd; int udp; int connecting; int listener; } g_s[HOST_SOCKETS];
static int g_init;
#ifdef HERMES_NET_SERVER
static int g_waiting[HOST_SOCKETS]; /* per server: a waiting client's slot + 1, 0: none */
#endif

static void init(void) {
    if (g_init) return;
    for (int i = 0; i < HOST_SOCKETS; i++) g_s[i].fd = -1;
    g_init = 1;
}

static int slot(int fd, int udp) {
    init();
    for (int i = 0; i < HOST_SOCKETS; i++)
        if (g_s[i].fd < 0) { g_s[i].fd = fd; g_s[i].udp = udp; g_s[i].connecting = !udp; g_s[i].listener = 0; return i; }
    close(fd);
    return -1;
}

static void addr(struct sockaddr_in *a, const uint8_t ip[4], uint16_t port) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    memcpy(&a->sin_addr.s_addr, ip, 4);
}

static int tcp_open(void *nif, const uint8_t ip[4], uint16_t port) {
    (void)nif;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in a;
    addr(&a, ip, port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    return slot(fd, 0);
}

static int udp_open(void *nif, uint16_t port) {
    (void)nif;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return slot(fd, 1);
}

static int status(void *nif, int s) {
    (void)nif;
    if (s < 0 || s >= HOST_SOCKETS || g_s[s].fd < 0) return HERMES_NET_CLOSED;
    if (g_s[s].udp) return HERMES_NET_OPEN_UDP;
    if (g_s[s].connecting) {
        struct pollfd p = { g_s[s].fd, POLLOUT, 0 };
        if (poll(&p, 1, 0) <= 0) return HERMES_NET_CONNECTING;
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(g_s[s].fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) return HERMES_NET_CLOSED;
        g_s[s].connecting = 0;
    }
    /* closed by the peer, with nothing left to read? */
    char c;
    ssize_t r = recv(g_s[s].fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 0) return HERMES_NET_CLOSED;
    return HERMES_NET_CONNECTED;
}

static int send_(void *nif, int s, const uint8_t *b, int n) {
    (void)nif;
    if (status(nif, s) != HERMES_NET_CONNECTED) return -1;
    ssize_t r = send(g_s[s].fd, b, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return (int)r;
}

static int send_to(void *nif, int s, const uint8_t ip[4], uint16_t port, const uint8_t *b, int n) {
    (void)nif;
    if (s < 0 || s >= HOST_SOCKETS || g_s[s].fd < 0) return -1;
    struct sockaddr_in a;
    addr(&a, ip, port);
    ssize_t r = sendto(g_s[s].fd, b, (size_t)n, 0, (struct sockaddr *)&a, sizeof(a));
    return r < 0 ? -1 : (int)r;
}

static int available(void *nif, int s) {
    (void)nif;
    if (s < 0 || s >= HOST_SOCKETS || g_s[s].fd < 0) return 0;
    int n = 0;
    if (ioctl(g_s[s].fd, FIONREAD, &n) < 0) return 0;
    return n;
}

static int recv_(void *nif, int s, uint8_t *b, int max) {
    (void)nif;
    if (s < 0 || s >= HOST_SOCKETS || g_s[s].fd < 0) return -1;
    ssize_t r = recv(g_s[s].fd, b, (size_t)max, MSG_DONTWAIT);
    return r < 0 ? 0 : (int)r;
}

static int recv_from(void *nif, int s, uint8_t *b, int max, uint8_t ip[4], uint16_t *port) {
    (void)nif;
    if (s < 0 || s >= HOST_SOCKETS || g_s[s].fd < 0) return -1;
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    uint8_t big[2048];
    ssize_t r = recvfrom(g_s[s].fd, big, sizeof(big), MSG_DONTWAIT, (struct sockaddr *)&a, &len);
    if (r < 0) return -1;
    int n = r > max ? max : (int)r;
    memcpy(b, big, (size_t)n);
    memcpy(ip, &a.sin_addr.s_addr, 4);
    *port = ntohs(a.sin_port);
    return n;
}

static void close_(void *nif, int s) {
    (void)nif;
    if (s < 0 || s >= HOST_SOCKETS || g_s[s].fd < 0) return;
    close(g_s[s].fd);
    g_s[s].fd = -1;
}

static void down(void *nif) {
    init();
    for (int i = 0; i < HOST_SOCKETS; i++) close_(nif, i);
#ifdef HERMES_NET_SERVER
    for (int i = 0; i < HOST_SOCKETS; i++) g_waiting[i] = 0;
#endif
}

#ifdef HERMES_NET_SERVER
/* A server is a listening socket in the same table; a client waiting is
 * taken with accept() and kept here until the program takes it. */
static int listen_(void *nif, uint16_t port) {
    (void)nif;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) { close(fd); return -1; }
    int s = slot(fd, 0);
    if (s >= 0) { g_s[s].connecting = 0; g_s[s].listener = 1; }
    return s;
}

static int accept_(void *nif, int srv, int take) {
    (void)nif;
    if (srv < 0 || srv >= HOST_SOCKETS || g_s[srv].fd < 0 || !g_s[srv].listener) return -1;
    if (!g_waiting[srv]) {
        int fd = accept(g_s[srv].fd, NULL, NULL);
        if (fd >= 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            int c = slot(fd, 0);
            if (c >= 0) { g_s[c].connecting = 0; g_waiting[srv] = c + 1; }
        }
    }
    int c = g_waiting[srv] - 1;
    if (c >= 0 && take) g_waiting[srv] = 0;
    return c;
}

static void unlisten(void *nif, int srv) {
    if (srv < 0 || srv >= HOST_SOCKETS) return;
    if (g_waiting[srv]) { close_(nif, g_waiting[srv] - 1); g_waiting[srv] = 0; }
    close_(nif, srv);
}

#endif

static int link_(void *nif) { (void)nif; return 1; }

/* the first IPv4 address that isn't the loopback, or 127.0.0.1 */
static int address(void *nif, uint8_t ip[4]) {
    (void)nif;
    struct ifaddrs *all, *i;
    uint8_t lo[4] = { 127, 0, 0, 1 };
    memcpy(ip, lo, 4);
    if (getifaddrs(&all) < 0) return 1;
    for (i = all; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        const uint8_t *a = (const uint8_t *)&((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
        if (a[0] == 127) continue;
        memcpy(ip, a, 4);
        break;
    }
    freeifaddrs(all);
    return 1;
}

#ifdef HERMES_NET_DNS
/* the system's resolver (its own DNS servers: set_dns isn't offered) */
static int resolve(void *nif, const char *name, uint8_t ip[4]) {
    (void)nif;
    struct addrinfo hint, *res;
    memset(&hint, 0, sizeof(hint));
    hint.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hint, &res) != 0) return 0;
    memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr, 4);
    freeaddrinfo(res);
    return 1;
}
#define HOST_RESOLVE resolve
#else
#define HOST_RESOLVE 0
#endif

#ifdef HERMES_NET_SERVER
#define HOST_SERVER listen_, accept_, unlisten
#else
#define HOST_SERVER 0, 0, 0
#endif

/* no DHCP: the PC has its address (dhcp() = has it one) */
const hermes_net_ops_t hermes_net_host_ops = {
    tcp_open, udp_open, status, send_, send_to, available, recv_, recv_from, close_, down,
    HOST_SERVER, link_, address, HOST_RESOLVE, 0, 0,
};
#endif
