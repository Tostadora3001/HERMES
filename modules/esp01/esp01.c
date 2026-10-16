/* The ESP-01 driver: Espressif's AT firmware (1.7 or later, 1.x syntax) as
 * a hermes_net interface (see esp01.h). Multiple links (AT+CIPMUX=1).
 *
 * TCP in the firmware's passive mode (AT+CIPRECVMODE=1, as WiFiEspAT): the
 * module keeps what arrives and says how much ("+IPD,<link>,<len>"); the
 * driver asks for it when the program reads (AT+CIPRECVDATA), a cacheful
 * at a time -- nothing is lost however slow the program is. UDP stays in
 * active mode (the firmware has no passive UDP in 1.x): datagrams come as
 * "+IPD,<link>,<len>,<ip>,<port>:<data>" (AT+CIPDINFO=1) and wait in one
 * ring shared by the UDP links.
 *
 * Every byte from the port goes through one parser (feed()). The port is
 * read only inside the interface's operations (hermes_net serializes
 * them); what arrives meanwhile waits in the UART driver's buffer. */
#include "esp01.h"
#include "hermes_drv_uart.h"
#include "hermes_drv_timer.h"
#include <string.h>

#if defined(__AVR__)
#include <avr/pgmspace.h>
#define TXT(s) PSTR(s)
#define TXT_BYTE(p) ((char)pgm_read_byte(p))
#define CACHE 32
#define UDP_RX 128
#else
#define TXT(s) (s)
#define TXT_BYTE(p) (*(p))
#define CACHE 256
#define UDP_RX 1024
#endif

#define LINKS 5
#define LINE_MAX 48
#define CMD_MS 2000
#define JOIN_MS 20000
#define DNS_MS 5000
#define STILL_SPINS 300000UL /* waits with the clock stopped (interrupts off): give up */

/* what ended a command */
enum { R_NONE, R_OK, R_ERROR, R_SEND_OK, R_SEND_FAIL, R_PROMPT, R_READY };

typedef struct {
    int port;
    uint8_t used, connected, udp; /* bit n: link n */
    uint8_t incoming;             /* links a client of our server opened, not accepted yet */
    uint8_t result, busy;
    uint8_t owed;                 /* a command's answer not waited for yet (a close) */
    uint8_t up, passive, server;  /* Wi-Fi joined; passive mode on; a server listening */
    uint8_t ip[4], found[4];      /* own address; the last name resolved */
    uint8_t got_ip, got_found;
    char line[LINE_MAX];
    uint8_t len;
    /* raw bytes still to come: active +IPD (UDP) or +CIPRECVDATA */
    uint16_t raw_left;
    uint8_t raw_kind;             /* 1: a datagram into the ring, 2: data into the cache */
    uint8_t raw_skip;             /* a datagram with no room: dropped */
    uint16_t avail[LINKS];        /* TCP bytes the module still holds */
    /* the cache: bytes of one link fetched from the module */
    signed char cache_link;
    uint16_t cache_pos, cache_len;
    uint8_t cache[CACHE];
    /* UDP datagrams: records [link][ip 4][port 2][length 2][data] */
    uint16_t head, tail;
    uint8_t ring[UDP_RX];
} esp01_t;

static esp01_t g_esp; /* one ESP-01 per program */

/* weak: on the host, esp01_mock.c puts a virtual module on that port */
extern void hermes_esp01_mock_attach(int port) __attribute__((weak));

/* --- the UDP ring ------------------------------------------------------------------ */
static uint16_t ring_used(const esp01_t *e) { return (uint16_t)((e->head + UDP_RX - e->tail) % UDP_RX); }
static uint16_t ring_room(const esp01_t *e) { return (uint16_t)(UDP_RX - 1 - ring_used(e)); }
static void ring_put(esp01_t *e, uint8_t b) {
    e->ring[e->head] = b;
    e->head = (uint16_t)((e->head + 1) % UDP_RX);
}
static uint8_t ring_at(const esp01_t *e, uint16_t i) { return e->ring[(e->tail + i) % UDP_RX]; }

/* --- the parser ---------------------------------------------------------------------- */
static int starts(const char *s, const char *p_txt) {
    for (; TXT_BYTE(p_txt); p_txt++, s++)
        if (*s != TXT_BYTE(p_txt)) return 0;
    return 1;
}

/* a number at *s, moving past it and one separator */
static long number(const char **s) {
    long v = 0;
    while (**s >= '0' && **s <= '9') v = v * 10 + (*(*s)++ - '0');
    if (**s) (*s)++;
    return v;
}

/* "a.b.c.d" (maybe in quotes) at s */
static void parse_ip(const char *s, uint8_t ip[4]) {
    if (*s == '"') s++;
    for (int i = 0; i < 4; i++) ip[i] = (uint8_t)number(&s);
}

/* "+IPD,<link>,<len>,<a.b.c.d>,<port>" with data after the ':' (active: UDP) */
static void ipd_data(esp01_t *e) {
    const char *s = e->line + 5;
    int link = (int)number(&s);
    long len = number(&s);
    uint8_t ip[4];
    for (int i = 0; i < 4; i++) ip[i] = (uint8_t)number(&s);
    uint16_t port = (uint16_t)number(&s);
    if (len <= 0) return;
    e->raw_left = (uint16_t)len;
    e->raw_kind = 1;
    e->raw_skip = link < 0 || link >= LINKS || ring_room(e) < 9 + len;
    if (e->raw_skip) return;
    ring_put(e, (uint8_t)link);
    for (int i = 0; i < 4; i++) ring_put(e, ip[i]);
    ring_put(e, (uint8_t)(port >> 8));
    ring_put(e, (uint8_t)port);
    ring_put(e, (uint8_t)(len >> 8));
    ring_put(e, (uint8_t)len);
}

static void line_done(esp01_t *e) {
    const char *l = e->line;
    if (!l[0]) return;
    if (starts(l, TXT("OK")) && !l[2]) e->result = R_OK;
    else if ((starts(l, TXT("ERROR")) && !l[5]) || (starts(l, TXT("FAIL")) && !l[4])) e->result = R_ERROR;
    else if (starts(l, TXT("SEND OK"))) e->result = R_SEND_OK;
    else if (starts(l, TXT("SEND FAIL"))) e->result = R_SEND_FAIL;
    else if (starts(l, TXT("ready"))) e->result = R_READY;
    else if (starts(l, TXT("busy"))) e->busy = 1;                   /* "busy p...": still working */
    else if (starts(l, TXT("WIFI GOT IP"))) e->up = 1;
    else if (starts(l, TXT("WIFI DISCONNECT"))) e->up = 0;
    else if (starts(l, TXT("+IPD,"))) {                              /* passive: how much it holds */
        const char *s = l + 5;
        int link = (int)number(&s);
        long len = number(&s);
        if (link >= 0 && link < LINKS) e->avail[link] = (uint16_t)len;
    } else if (starts(l, TXT("+CIPDOMAIN:"))) {
        parse_ip(l + 11, e->found);
        e->got_found = 1;
    } else if (starts(l, TXT("+CIPSTA:ip:"))) {
        parse_ip(l + 11, e->ip);
        e->got_ip = 1;
    } else if (l[0] >= '0' && l[0] < '0' + LINKS && l[1] == ',') {
        uint8_t bit = (uint8_t)(1u << (l[0] - '0'));
        if (starts(l + 2, TXT("CONNECT FAIL")) || starts(l + 2, TXT("CLOSED"))) {
            e->connected &= (uint8_t)~bit;
            e->incoming &= (uint8_t)~bit;
        } else if (starts(l + 2, TXT("CONNECT"))) {
            e->connected |= bit;
            if (!(e->used & bit)) e->incoming |= bit; /* a client of our server */
        }
    }
    /* anything else (echo, WIFI CONNECTED, Recv n bytes...): ignored */
}

static void feed(esp01_t *e, uint8_t b) {
    if (e->raw_left) {
        if (e->raw_kind == 1) {
            if (!e->raw_skip) ring_put(e, b);
        } else if (e->cache_len < CACHE) {
            e->cache[e->cache_len++] = b;
        }
        e->raw_left--;
        return;
    }
    if (b == '>' && e->len == 0) { e->result = R_PROMPT; return; }
    if (b == '\r') return;
    if (b == '\n') {
        e->line[e->len] = 0;
        e->len = 0;
        line_done(e);
        return;
    }
    if (b == ':' && e->len >= 5) {
        e->line[e->len] = 0;
        if (starts(e->line, TXT("+IPD,"))) { e->len = 0; ipd_data(e); return; }
        if (starts(e->line, TXT("+CIPRECVDATA,"))) {               /* "+CIPRECVDATA,<n>:<data>" */
            const char *s = e->line + 13;
            e->raw_left = (uint16_t)number(&s);
            e->raw_kind = 2;
            e->len = 0;
            return;
        }
    }
    if (e->len < LINE_MAX - 1) e->line[e->len++] = (char)b;
}

static void pump(esp01_t *e) {
    int b;
    while ((b = hermes_uart_port_read_byte(e->port)) >= 0) feed(e, (uint8_t)b);
}

/* --- commands ----------------------------------------------------------------------- */
static void put_c(esp01_t *e, char c) { hermes_uart_port_write_byte(e->port, (uint8_t)c); }
static void put_txt(esp01_t *e, const char *p_txt) {
    for (; TXT_BYTE(p_txt); p_txt++) put_c(e, TXT_BYTE(p_txt));
}
static void put_str(esp01_t *e, const char *s) {
    while (*s) put_c(e, *s++);
}
static void put_num(esp01_t *e, long v) {
    char d[11];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n) put_c(e, d[--n]);
}
static void put_ip(esp01_t *e, const uint8_t ip[4]) {
    char t[16];
    put_c(e, '"');
    put_str(e, hermes_net_ip_text(ip, t));
    put_c(e, '"');
}

/* Waits until one of the results in `want` (bits), or ms. "busy p..."
 * starts the time again (the module is still at it). With `probe`, a
 * silent module is asked "?" up to twice, as WiFiEspAT does: its ERROR
 * says an answer was missed (the command failed), silence that it's gone.
 * Never while it waits for the bytes of a CIPSEND: "?" would be data.
 * If the clock doesn't move (interrupts off: an abort under the
 * framework's lock), it gives up after STILL_SPINS turns. */
static uint8_t wait_for(esp01_t *e, uint8_t want, uint32_t ms, int probe) {
    uint32_t start = hermes_timer_millis(), last = start;
    unsigned long still = 0;
    int probes = 0;
    for (;;) {
        pump(e);
        if (e->result && (want & (1u << e->result))) return e->result;
        uint32_t now = hermes_timer_millis();
        if (e->busy) { e->busy = 0; start = now; }
        if (now != last) { last = now; still = 0; }
        else if (++still > STILL_SPINS) return R_NONE;
        if (now - start >= ms) {
            if (!probe || probes >= 2) return R_NONE;
            probes++;
            put_txt(e, TXT("?\r\n"));
            start = now;
        }
    }
}
#define DONE ((uint8_t)(1u << R_OK | 1u << R_ERROR))

/* a command's start: first the answer still owed to the last one (a
 * close: see close_), then its text */
static void cmd_start(esp01_t *e, const char *p_txt) {
    if (e->owed) {
        e->result = R_NONE;
        wait_for(e, DONE, CMD_MS, 0);
        e->owed = 0;
    }
    put_txt(e, p_txt);
}

/* the end of a command line, and its result */
static uint8_t end_cmd(esp01_t *e, uint8_t want, uint32_t ms, int probe) {
    pump(e);         /* what was already there isn't this command's answer */
    e->result = R_NONE;
    put_c(e, '\r');
    put_c(e, '\n');
    return wait_for(e, want, ms, probe);
}

static uint8_t simple(esp01_t *e, const char *p_txt) {
    cmd_start(e, p_txt);
    return end_cmd(e, DONE, CMD_MS, 1);
}

void *hermes_esp01_init(int port, const char *ssid, const char *pass) {
    esp01_t *e = &g_esp;
    memset(e, 0, sizeof(*e));
    e->port = port;
    e->cache_link = -1;
    if (hermes_esp01_mock_attach) hermes_esp01_mock_attach(port);
    int alive = 0;
    for (int i = 0; i < 3 && !alive; i++) { /* the first AT may meet a module still booting */
        cmd_start(e, TXT("AT"));
        alive = end_cmd(e, DONE, 500, 0) == R_OK;
    }
    if (!alive) return 0;
    /* A reset first (as WiFiEspAT does): the module keeps its links when
     * only the microcontroller restarts, and with links open the firmware
     * refuses AT+CIPMUX. "ready" may be missed (boot messages at another
     * speed): then it goes on after 3 s. */
    cmd_start(e, TXT("AT+RST"));
    end_cmd(e, (uint8_t)(1u << R_READY), 3000, 0);
    simple(e, TXT("ATE0"));
    simple(e, TXT("AT+CWMODE=1"));
    cmd_start(e, TXT("AT+CWJAP=\""));
    put_str(e, ssid);
    put_txt(e, TXT("\",\""));
    put_str(e, pass);
    put_c(e, '"');
    if (end_cmd(e, DONE, JOIN_MS, 0) == R_OK) e->up = 1;
    else HERMES_DIAG("esp01: couldn't join the Wi-Fi network \"%s\" (its sockets will fail)\n", ssid);
    simple(e, TXT("AT+CIPMUX=1"));
    simple(e, TXT("AT+CIPDINFO=1"));
    e->passive = simple(e, TXT("AT+CIPRECVMODE=1")) == R_OK;
    if (!e->passive) HERMES_DIAG("esp01: no passive receive mode (AT firmware 1.7 or later needed): TCP data won't be read\n");
    return e;
}

/* outgoing links from the top down: the firmware gives a server's clients
 * the lowest free ones (as WiFiEspAT) */
static int take(esp01_t *e) {
    for (int l = LINKS - 1; l >= 0; l--) {
        uint8_t bit = (uint8_t)(1u << l);
        if (!(e->used & bit) && !(e->incoming & bit) && !(e->connected & bit)) {
            e->used |= bit;
            e->avail[l] = 0;
            return l;
        }
    }
    return -1;
}
static void give_back(esp01_t *e, int l) {
    uint8_t bit = (uint8_t)(1u << l);
    e->used &= (uint8_t)~bit;
    e->connected &= (uint8_t)~bit;
    e->udp &= (uint8_t)~bit;
    e->avail[l] = 0;
    if (e->cache_link == l) e->cache_link = -1;
}

/* A failed AT+CIPSTART may have opened the link anyway (its answer lost:
 * ERROR came from the "?"), so the link is closed and it's tried once
 * more. A real failure (a closed port) just fails twice. */
static void unstart(esp01_t *e, int l) {
    cmd_start(e, TXT("AT+CIPCLOSE="));
    put_num(e, l);
    end_cmd(e, DONE, CMD_MS, 1);
}

static int tcp_open(void *nif, const uint8_t ip[4], uint16_t port) {
    esp01_t *e = nif;
    int l = take(e);
    if (l < 0) return -1;
    for (int tries = 0; tries < 2; tries++) {
        cmd_start(e, TXT("AT+CIPSTART="));
        put_num(e, l);
        put_txt(e, TXT(",\"TCP\","));
        put_ip(e, ip);
        put_c(e, ',');
        put_num(e, port);
        if (end_cmd(e, DONE, HERMES_NET_CONNECT_MS, 1) == R_OK) {
            e->connected |= (uint8_t)(1u << l); /* "n,CONNECT" came before the OK */
            return l;
        }
        unstart(e, l);
    }
    give_back(e, l);
    return -1;
}

/* The remote end of a UDP link is given per datagram (AT+CIPSEND with an
 * address): the link opens towards "0.0.0.0" in mode 2 ("the remote may
 * change"), with the local port also as the remote one -- what WiFiEspAT
 * does for WiFiUDP::begin (a remote port 0 may be refused). */
static int udp_open(void *nif, uint16_t port) {
    esp01_t *e = nif;
    int l = take(e);
    if (l < 0) return -1;
    for (int tries = 0; tries < 2; tries++) {
        cmd_start(e, TXT("AT+CIPSTART="));
        put_num(e, l);
        put_txt(e, TXT(",\"UDP\",\"0.0.0.0\","));
        put_num(e, port);
        put_c(e, ',');
        put_num(e, port);
        put_txt(e, TXT(",2"));
        if (end_cmd(e, DONE, CMD_MS, 1) == R_OK) {
            e->udp |= (uint8_t)(1u << l);
            return l;
        }
        unstart(e, l);
    }
    give_back(e, l);
    return -1;
}

static int available(void *nif, int l) {
    esp01_t *e = nif;
    pump(e);
    return e->avail[l] + (e->cache_link == l ? e->cache_len - e->cache_pos : 0);
}

static int status(void *nif, int l) {
    esp01_t *e = nif;
    uint8_t bit = (uint8_t)(1u << l);
    if (e->udp & bit) return HERMES_NET_OPEN_UDP;
    if ((e->connected & bit) || available(nif, l)) return HERMES_NET_CONNECTED; /* what's left is readable */
    return HERMES_NET_CLOSED;
}

static int cipsend(esp01_t *e, int l, const uint8_t *ip, uint16_t port, const uint8_t *b, int n) {
    if (n <= 0) return 0;
    if (n > 2048) n = 2048; /* the firmware's limit per command */
    cmd_start(e, TXT("AT+CIPSEND="));
    put_num(e, l);
    put_c(e, ',');
    put_num(e, n);
    if (ip) {
        put_c(e, ',');
        put_ip(e, ip);
        put_c(e, ',');
        put_num(e, port);
    }
    /* A lost "> " (no answer at all) may leave the module waiting for the
     * n bytes: they are sent anyway. If it was waiting, they go out; if
     * not, they make a bad command line that the "?" of wait_for ends
     * with ERROR. Either way the module is back to commands. */
    if (end_cmd(e, (uint8_t)(1u << R_PROMPT | 1u << R_ERROR), CMD_MS, 0) == R_ERROR) return -1;
    e->result = R_NONE;
    for (int i = 0; i < n; i++) put_c(e, (char)b[i]);
    return wait_for(e, (uint8_t)(1u << R_SEND_OK | 1u << R_SEND_FAIL | 1u << R_ERROR), CMD_MS, 1) == R_SEND_OK ? n : -1;
}

static int send_(void *nif, int l, const uint8_t *b, int n) {
    esp01_t *e = nif;
    pump(e);
    if (!(e->connected & (1u << l))) return -1;
    return cipsend(e, l, 0, 0, b, n);
}

static int send_to(void *nif, int l, const uint8_t ip[4], uint16_t port, const uint8_t *b, int n) {
    return cipsend(nif, l, ip, port, b, n);
}

/* from the cache, which is filled from the module a cacheful at a time */
static int recv_(void *nif, int l, uint8_t *b, int max) {
    esp01_t *e = nif;
    pump(e);
    if ((e->cache_link != l || e->cache_pos == e->cache_len) && e->avail[l]) {
        uint16_t ask = e->avail[l] > CACHE ? CACHE : e->avail[l];
        e->cache_link = (signed char)l;
        e->cache_pos = e->cache_len = 0;
        cmd_start(e, TXT("AT+CIPRECVDATA="));
        put_num(e, l);
        put_c(e, ',');
        put_num(e, ask);
        if (end_cmd(e, DONE, CMD_MS, 1) == R_OK) {
            /* "+IPD,<link>,<len>" gives what the module holds (as WiFiEspAT
             * takes it); a notice may cross this read, so after a full
             * cacheful at least 1 stays: the next read asks again */
            e->avail[l] = e->cache_len >= e->avail[l] ? 0 : (uint16_t)(e->avail[l] - e->cache_len);
            if (e->cache_len == ask && !e->avail[l]) e->avail[l] = 1;
        } else {
            e->avail[l] = 0;    /* the module has nothing more (a closed link) */
        }
    }
    int n = 0;
    if (e->cache_link == l)
        while (n < max && e->cache_pos < e->cache_len) b[n++] = e->cache[e->cache_pos++];
    return n;
}

static int recv_from(void *nif, int l, uint8_t *b, int max, uint8_t ip[4], uint16_t *port) {
    esp01_t *e = nif;
    pump(e);
    /* the oldest datagram of this link (the ring is shared: others are skipped over) */
    uint16_t used = ring_used(e), at = 0;
    if (e->raw_left && e->raw_kind == 1) return -1; /* one still arriving */
    while (at + 9 <= used) {
        uint16_t len = (uint16_t)(ring_at(e, (uint16_t)(at + 7)) << 8 | ring_at(e, (uint16_t)(at + 8)));
        if (ring_at(e, at) != l) { at = (uint16_t)(at + 9 + len); continue; }
        for (int i = 0; i < 4; i++) ip[i] = ring_at(e, (uint16_t)(at + 1 + i));
        *port = (uint16_t)(ring_at(e, (uint16_t)(at + 5)) << 8 | ring_at(e, (uint16_t)(at + 6)));
        int n = 0;
        for (uint16_t i = 0; i < len; i++)
            if (n < max) b[n++] = ring_at(e, (uint16_t)(at + 9 + i)); /* a longer datagram's rest is skipped */
        /* out of the ring: the records before it move up by its size */
        uint16_t size = (uint16_t)(9 + len);
        for (uint16_t i = at; i > 0; i--)
            e->ring[(e->tail + i - 1 + size) % UDP_RX] = e->ring[(e->tail + i - 1) % UDP_RX];
        e->tail = (uint16_t)((e->tail + size) % UDP_RX);
        return n;
    }
    return -1;
}

/* Closing doesn't wait for the module's answer -- the next command
 * collects it first (cmd_start): a close is also an abort, and aborts run
 * with the framework's lock held (on bare metal, interrupts off: no millis,
 * no receiving), so waiting there would never end. */
static void close_(void *nif, int l) {
    esp01_t *e = nif;
    if (!(e->used & (1u << l))) return;
    cmd_start(e, TXT("AT+CIPCLOSE="));
    put_num(e, l);
    put_c(e, '\r');
    put_c(e, '\n');
    e->owed = 1;
    give_back(e, l);
}

#ifdef HERMES_NET_SERVER
/* --- the server: AT+CIPSERVER (one per module, on CIPMUX=1); its clients
 *     open links of their own ("n,CONNECT" for a link we didn't open) ------------- */
static int listen_(void *nif, uint16_t port) {
    esp01_t *e = nif;
    if (e->server) return -1; /* the firmware has one server */
    for (int tries = 0; tries < 2 && !e->server; tries++) { /* again if its answer got lost */
        cmd_start(e, TXT("AT+CIPSERVER=1,"));
        put_num(e, port);
        e->server = end_cmd(e, DONE, CMD_MS, 1) == R_OK;
    }
    return e->server ? 0 : -1;
}

static int accept_(void *nif, int srv, int take) {
    esp01_t *e = nif;
    (void)srv;
    pump(e);
    for (int l = 0; l < LINKS; l++) {
        uint8_t bit = (uint8_t)(1u << l);
        if (!(e->incoming & bit)) continue;
        if (take) {
            e->incoming &= (uint8_t)~bit;
            e->used |= bit;
        }
        return l;
    }
    return -1;
}

static void unlisten(void *nif, int srv) {
    esp01_t *e = nif;
    (void)srv;
    if (!e->server) return;
    cmd_start(e, TXT("AT+CIPSERVER=0"));
    put_c(e, '\r');
    put_c(e, '\n');
    e->owed = 1; /* not waited for: an abort (see close_) */
    e->server = 0;
    e->incoming = 0;
}
#define E_SERVER listen_, accept_, unlisten
#else
#define E_SERVER 0, 0, 0
#endif

/* every link at once: AT+CIPCLOSE=5 (the firmware's "all") */
static void down(void *nif) {
    esp01_t *e = nif;
#ifdef HERMES_NET_SERVER
    if (e->server) unlisten(nif, 0);
#endif
    if (!e->used) return;
    put_txt(e, TXT("AT+CIPCLOSE=5\r\n"));
    e->owed = 1;
    for (int l = 0; l < LINKS; l++) give_back(e, l);
}

/* --- link, address, DNS -------------------------------------------------------------- */
static int link_(void *nif) {
    esp01_t *e = nif;
    pump(e);
    return e->up;
}

static int address(void *nif, uint8_t ip[4]) {
    esp01_t *e = nif;
    e->got_ip = 0;
    if (simple(e, TXT("AT+CIPSTA?")) != R_OK || !e->got_ip) return 0;
    for (int i = 0; i < 4; i++) ip[i] = e->ip[i];
    return ip[0] || ip[1] || ip[2] || ip[3];
}

#ifdef HERMES_NET_DNS
static int resolve(void *nif, const char *name, uint8_t ip[4]) {
    esp01_t *e = nif;
    e->got_found = 0;
    cmd_start(e, TXT("AT+CIPDOMAIN=\""));
    put_str(e, name);
    put_c(e, '"');
    if (end_cmd(e, DONE, DNS_MS, 1) != R_OK || !e->got_found) return 0;
    for (int i = 0; i < 4; i++) ip[i] = e->found[i];
    return 1;
}

static int set_dns(void *nif, const uint8_t ip[4]) {
    esp01_t *e = nif;
    cmd_start(e, TXT("AT+CIPDNS_CUR=1,"));
    put_ip(e, ip);
    return end_cmd(e, DONE, CMD_MS, 1) == R_OK;
}
#define E_DNS resolve, set_dns
#else
#define E_DNS 0, 0
#endif

/* no dhcp operation: the module does DHCP itself (dhcp() = has it an address) */
const hermes_net_ops_t hermes_esp01_ops = {
    tcp_open, udp_open, status, send_, send_to, available, recv_, recv_from, close_, down,
    E_SERVER, link_, address, E_DNS, 0,
};
