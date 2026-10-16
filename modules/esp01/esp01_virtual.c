/* A virtual ESP-01: see esp01_virtual.h. Commands understood (Espressif AT
 * firmware 1.x syntax), each line ending in \r\n, echoed back until ATE0:
 *   AT, ATE0, ATE1, AT+RST, AT+CWMODE=<m>, AT+CWJAP="ssid","pass",
 *   AT+CIPMUX=1, AT+CIPDINFO=1,
 *   AT+CIPSTART=<id>,"TCP","<ip>",<port>
 *   AT+CIPSTART=<id>,"UDP","<ip>",<port>,<local port>,2
 *   AT+CIPSEND=<id>,<len>[,"<ip>",<port>]  -> "> ", then <len> raw bytes
 *   AT+CIPCLOSE=<id>, AT+CIPCLOSE=5 (all)
 *   AT+CIPRECVMODE=1 (passive TCP), AT+CIPRECVDATA=<id>,<len>
 *   AT+CIPSERVER=1,<port>, AT+CIPSERVER=0
 *   AT+CIPDOMAIN="<name>" (the PC's resolver), AT+CIPSTA? (10.0.0.3),
 *   AT+CIPDNS_CUR=1,"<ip>" (accepted, not used)
 * Anything else ("?" included): ERROR. Incoming data, active mode (UDP
 * always): "+IPD,<id>,<len>,<ip>,<port>:<data>" (CIPDINFO=1); passive TCP:
 * the data stays here and "+IPD,<id>,<total held>" says so, and
 * AT+CIPRECVDATA answers "+CIPRECVDATA,<n>:<data>" (AT 1.7's form). A peer
 * that closes: "<id>,CLOSED" (what it sent stays readable); a client of
 * the server: "<id>,CONNECT" on the lowest free link. Five links (0-4).
 *
 * For testing a driver's recovery: with ESP01V_SILENT=<text> in the
 * environment, the first command starting with that text is done but not
 * answered (as a module whose answer got lost), and with ESP01V_BUSY=<text>
 * the first one gets "busy p..." lines for about a second before its
 * answer. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "esp01_virtual.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LINKS 5
#define OUT 8192
#define HOLD 4096

struct esp01v {
    int echo;
    char line[256];
    int len;
    /* CIPSEND: raw bytes still to come for link `send_id` */
    int send_left, send_id, send_n;
    uint8_t send_buf[2048];
    struct sockaddr_in send_to;
    int send_to_set;
    struct { int fd, udp, held; uint8_t hold[HOLD]; } link[LINKS];
    int passive, server_fd;
    int silent_done, busy_done;
    double busy_until;     /* "busy p..." until then, then the answer held back */
    char held_answer[256];
    uint8_t out[OUT];
    unsigned head, tail;
};

static void out_bytes(esp01v_t *v, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        unsigned next = (v->head + 1) % OUT;
        if (next == v->tail) return; /* full: dropped */
        v->out[v->head] = b[i];
        v->head = next;
    }
}
static void out_str(esp01v_t *v, const char *s) { out_bytes(v, s, strlen(s)); }

esp01v_t *esp01v_new(void) {
    esp01v_t *v = calloc(1, sizeof(*v));
    v->echo = 1;
    v->server_fd = -1;
    for (int i = 0; i < LINKS; i++) v->link[i].fd = -1;
    return v;
}

static void link_close(esp01v_t *v, int id) {
    if (v->link[id].fd >= 0) close(v->link[id].fd);
    v->link[id].fd = -1;
    v->link[id].held = 0;
}

static void server_close(esp01v_t *v) {
    if (v->server_fd >= 0) close(v->server_fd);
    v->server_fd = -1;
}

/* "a.b.c.d" (quoted in the command) and a port into an address */
static int parse_addr(const char *ip, int port, struct sockaddr_in *a) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, ip, &a->sin_addr) == 1;
}

static void command(esp01v_t *v, char *c) {
    char ip[64], proto[8];
    int id, port, local = 0, mode, n;
    if (!strcmp(c, "AT")) { out_str(v, "\r\nOK\r\n"); return; }
    if (!strcmp(c, "ATE0")) { v->echo = 0; out_str(v, "\r\nOK\r\n"); return; }
    if (!strcmp(c, "ATE1")) { v->echo = 1; out_str(v, "\r\nOK\r\n"); return; }
    if (!strcmp(c, "AT+RST")) {
        for (int i = 0; i < LINKS; i++) link_close(v, i);
        server_close(v);
        v->passive = 0;
        v->echo = 1; /* as after power-up */
        out_str(v, "\r\nOK\r\n\r\nready\r\n");
        return;
    }
    if (!strcmp(c, "AT+CIPMUX=1")) { /* refused while links are open, as the firmware */
        for (int i = 0; i < LINKS; i++)
            if (v->link[i].fd >= 0) { out_str(v, "link is builded\r\n\r\nERROR\r\n"); return; }
    }
    if (!strncmp(c, "AT+CWMODE=", 10) || !strcmp(c, "AT+CIPMUX=1") || !strcmp(c, "AT+CIPDINFO=1")) {
        out_str(v, "\r\nOK\r\n");
        return;
    }
    if (!strcmp(c, "AT+CIPRECVMODE=1")) { v->passive = 1; out_str(v, "\r\nOK\r\n"); return; }
    if (sscanf(c, "AT+CIPRECVDATA=%d,%d", &id, &n) == 2 && id >= 0 && id < LINKS && n > 0) {
        int k = n < v->link[id].held ? n : v->link[id].held;
        if (!k && v->link[id].fd < 0) { out_str(v, "\r\nERROR\r\n"); return; }
        char m[32];
        snprintf(m, sizeof(m), "+CIPRECVDATA,%d:", k);
        out_str(v, m);
        out_bytes(v, v->link[id].hold, (size_t)k);
        memmove(v->link[id].hold, v->link[id].hold + k, (size_t)(v->link[id].held - k));
        v->link[id].held -= k;
        out_str(v, "\r\nOK\r\n");
        return;
    }
    if (sscanf(c, "AT+CIPSERVER=1,%d", &port) == 1) {
        server_close(v);
        int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        struct sockaddr_in a;
        parse_addr("127.0.0.1", port, &a);
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) {
            close(fd);
            out_str(v, "\r\nERROR\r\n");
            return;
        }
        v->server_fd = fd;
        out_str(v, "\r\nOK\r\n");
        return;
    }
    if (!strcmp(c, "AT+CIPSERVER=0")) { server_close(v); out_str(v, "\r\nOK\r\n"); return; }
    if (sscanf(c, "AT+CIPDOMAIN=\"%63[^\"]\"", ip) == 1) {
        struct addrinfo hint, *res;
        memset(&hint, 0, sizeof(hint));
        hint.ai_family = AF_INET;
        if (getaddrinfo(ip, NULL, &hint, &res) != 0) { out_str(v, "DNS Fail\r\n\r\nERROR\r\n"); return; }
        char t[INET_ADDRSTRLEN], m[64];
        inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, t, sizeof(t));
        freeaddrinfo(res);
        snprintf(m, sizeof(m), "+CIPDOMAIN:%s\r\n\r\nOK\r\n", t);
        out_str(v, m);
        return;
    }
    if (!strcmp(c, "AT+CIPSTA?")) {
        out_str(v, "+CIPSTA:ip:\"10.0.0.3\"\r\n+CIPSTA:gateway:\"10.0.0.1\"\r\n+CIPSTA:netmask:\"255.255.255.0\"\r\n\r\nOK\r\n");
        return;
    }
    if (!strncmp(c, "AT+CIPDNS_CUR=1,", 16)) { out_str(v, "\r\nOK\r\n"); return; }
    if (!strncmp(c, "AT+CWJAP=", 9)) {
        out_str(v, "WIFI CONNECTED\r\nWIFI GOT IP\r\n\r\nOK\r\n");
        return;
    }
    if (sscanf(c, "AT+CIPSTART=%d,\"%7[^\"]\",\"%63[^\"]\",%d,%d,%d", &id, proto, ip, &port, &local, &mode) >= 4) {
        struct sockaddr_in a;
        if (id < 0 || id >= LINKS || v->link[id].fd >= 0) { out_str(v, "\r\nALREADY CONNECTED\r\n\r\nERROR\r\n"); return; }
        if (!strcmp(proto, "TCP")) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (!parse_addr(ip, port, &a) || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
                close(fd);
                char m[32];
                snprintf(m, sizeof(m), "%d,CLOSED\r\n\r\nERROR\r\n", id);
                out_str(v, m);
                return;
            }
            v->link[id].fd = fd;
            v->link[id].udp = 0;
            v->link[id].held = 0;
        } else if (!strcmp(proto, "UDP")) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in b;
            memset(&b, 0, sizeof(b));
            b.sin_family = AF_INET;
            b.sin_port = htons((uint16_t)(local > 0 ? local : 0)); /* its local port, if given */
            if (bind(fd, (struct sockaddr *)&b, sizeof(b)) < 0) {
                close(fd);
                out_str(v, "\r\nERROR\r\n");
                return;
            }
            v->link[id].fd = fd;
            v->link[id].udp = 1;
        } else {
            out_str(v, "\r\nERROR\r\n");
            return;
        }
        char m[32];
        snprintf(m, sizeof(m), "%d,CONNECT\r\n\r\nOK\r\n", id);
        out_str(v, m);
        return;
    }
    if (sscanf(c, "AT+CIPSEND=%d,%d", &id, &n) == 2) {
        if (id < 0 || id >= LINKS || v->link[id].fd < 0 || n <= 0 || n > (int)sizeof(v->send_buf)) {
            out_str(v, "\r\nlink is not valid\r\n\r\nERROR\r\n");
            return;
        }
        v->send_to_set = sscanf(c, "AT+CIPSEND=%d,%d,\"%63[^\"]\",%d", &id, &n, ip, &port) == 4 &&
                         parse_addr(ip, port, &v->send_to);
        v->send_id = id;
        v->send_left = v->send_n = n;
        out_str(v, "\r\nOK\r\n> ");
        return;
    }
    if (!strcmp(c, "AT+CIPCLOSE=5")) { /* all of them */
        for (int i = 0; i < LINKS; i++) link_close(v, i);
        out_str(v, "\r\nOK\r\n");
        return;
    }
    if (sscanf(c, "AT+CIPCLOSE=%d", &id) == 1 && id >= 0 && id < LINKS) {
        link_close(v, id);
        char m[32];
        snprintf(m, sizeof(m), "%d,CLOSED\r\n\r\nOK\r\n", id);
        out_str(v, m);
        return;
    }
    out_str(v, "\r\nERROR\r\n");
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

/* ESP01V_SILENT / ESP01V_BUSY (see the top) */
static void command_tested(esp01v_t *v, char *c) {
    const char *silent = getenv("ESP01V_SILENT"), *busy = getenv("ESP01V_BUSY");
    if (silent && !v->silent_done && !strncmp(c, silent, strlen(silent))) {
        v->silent_done = 1;
        unsigned head = v->head;
        command(v, c);
        v->head = head; /* done, its answer lost */
        return;
    }
    if (busy && !v->busy_done && !strncmp(c, busy, strlen(busy))) {
        v->busy_done = 1;
        unsigned head = v->head;
        command(v, c);
        size_t n = 0; /* the answer, held back */
        for (unsigned i = head; i != v->head && n < sizeof(v->held_answer) - 1; i = (i + 1) % OUT) v->held_answer[n++] = (char)v->out[i];
        v->held_answer[n] = 0;
        v->head = head;
        v->busy_until = now_s() + 1.0;
        return;
    }
    command(v, c);
}

void esp01v_input(esp01v_t *v, uint8_t b) {
    if (v->send_left > 0) { /* the raw bytes of a CIPSEND */
        v->send_buf[v->send_n - v->send_left] = b;
        if (--v->send_left == 0) {
            int id = v->send_id, fd = v->link[id].fd;
            ssize_t r = -1;
            if (fd >= 0 && v->link[id].udp && v->send_to_set)
                r = sendto(fd, v->send_buf, (size_t)v->send_n, 0, (struct sockaddr *)&v->send_to, sizeof(v->send_to));
            else if (fd >= 0 && !v->link[id].udp)
                r = send(fd, v->send_buf, (size_t)v->send_n, MSG_NOSIGNAL);
            char m[48];
            snprintf(m, sizeof(m), "\r\nRecv %d bytes\r\n\r\n%s\r\n", v->send_n, r == v->send_n ? "SEND OK" : "SEND FAIL");
            out_str(v, m);
        }
        return;
    }
    if (v->echo) out_bytes(v, &b, 1);
    if (b == '\r') return;
    if (b != '\n') {
        if (v->len < (int)sizeof(v->line) - 1) v->line[v->len++] = (char)b;
        return;
    }
    v->line[v->len] = 0;
    v->len = 0;
    if (v->line[0]) command_tested(v, v->line);
}

/* data from the host's sockets: +IPD messages; closed peers; the
 * server's new clients */
static void poll_links(esp01v_t *v) {
    if (v->server_fd >= 0) {
        int id = 0;
        while (id < LINKS && (v->link[id].fd >= 0 || v->link[id].held)) id++;
        int fd = id < LINKS ? accept(v->server_fd, NULL, NULL) : -1;
        if (fd >= 0) {
            v->link[id].fd = fd;
            v->link[id].udp = 0;
            char m[24];
            snprintf(m, sizeof(m), "%d,CONNECT\r\n", id);
            out_str(v, m);
        }
    }
    for (int id = 0; id < LINKS; id++) {
        if (v->link[id].fd < 0) continue;
        if (v->passive && !v->link[id].udp) { /* passive: kept here, and said how much */
            int room = HOLD - v->link[id].held;
            if (room <= 0) continue;
            ssize_t n = recv(v->link[id].fd, v->link[id].hold + v->link[id].held, (size_t)room, MSG_DONTWAIT);
            char m[32];
            if (n == 0) {
                close(v->link[id].fd);
                v->link[id].fd = -1; /* what it holds stays readable */
                snprintf(m, sizeof(m), "%d,CLOSED\r\n", id);
                out_str(v, m);
            } else if (n > 0) {
                v->link[id].held += (int)n;
                snprintf(m, sizeof(m), "\r\n+IPD,%d,%d\r\n", id, v->link[id].held);
                out_str(v, m);
            }
            continue;
        }
        uint8_t buf[1024];
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        memset(&a, 0, sizeof(a));
        ssize_t n = v->link[id].udp ? recvfrom(v->link[id].fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&a, &al)
                                    : recv(v->link[id].fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (!v->link[id].udp) getpeername(v->link[id].fd, (struct sockaddr *)&a, &al);
        if (n == 0 && !v->link[id].udp) {
            link_close(v, id);
            char m[24];
            snprintf(m, sizeof(m), "%d,CLOSED\r\n", id);
            out_str(v, m);
            continue;
        }
        if (n <= 0) continue;
        char ip[INET_ADDRSTRLEN], h[80];
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        snprintf(h, sizeof(h), "\r\n+IPD,%d,%d,%s,%d:", id, (int)n, ip, ntohs(a.sin_port));
        out_str(v, h);
        out_bytes(v, buf, (size_t)n);
    }
}

int esp01v_output(esp01v_t *v) {
    if (v->busy_until > 0 && v->tail == v->head) {
        static double last;
        double t = now_s();
        if (t >= v->busy_until) { v->busy_until = 0; out_str(v, v->held_answer); }
        else if (t - last > 0.2) { last = t; out_str(v, "busy p...\r\n"); }
        return -1;
    }
    if (v->tail == v->head) poll_links(v);
    if (v->tail == v->head) return -1;
    int b = v->out[v->tail];
    v->tail = (v->tail + 1) % OUT;
    return b;
}
