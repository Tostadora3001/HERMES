/* The HM-10 driver: see hm10.h. Every byte from the port goes through
 * one filter (take()) that takes the connection notices out of the data;
 * the port is read only inside the device's methods. */
#include "hm10.h"
#include "hermes_drv_uart.h"
#include "hermes_drv_timer.h"

#define HOLD_MS 20
#define AT_MS 500

/* weak: on the host, hm10_mock.c puts a virtual HM-10 on that port */
extern void hermes_hm10_mock_attach(int port) __attribute__((weak));

static const char CONN[] = "OK+CONN", LOST[] = "OK+LOST";

static int prefix(const char *hold, int n, const char *notice) {
    for (int i = 0; i < n; i++)
        if (!notice[i] || hold[i] != notice[i]) return 0;
    return 1;
}

static void store(hermes_device_hm10_t *b, uint8_t c) {
    uint16_t next = (uint16_t)((b->head + 1) % HERMES_HM10_RX);
    if (next == b->tail) return; /* full: lost */
    b->rx[b->head] = c;
    b->head = next;
}

/* the held bytes that can't start a notice any more go to the data */
static void release(hermes_device_hm10_t *b, int all) {
    while (b->hold_len && (all || (!prefix(b->hold, b->hold_len, CONN) && !prefix(b->hold, b->hold_len, LOST)))) {
        store(b, (uint8_t)b->hold[0]);
        for (int i = 1; i < b->hold_len; i++) b->hold[i - 1] = b->hold[i];
        b->hold_len--;
        all = 0;
    }
}

static void take(hermes_device_hm10_t *b, uint8_t c) {
    if (b->hold_len == sizeof(b->hold)) release(b, 1);
    b->hold[b->hold_len++] = (char)c;
    b->hold_ms = hermes_timer_millis();
    if (b->hold_len == 7 && prefix(b->hold, 7, CONN)) { b->connected = 1; b->hold_len = 0; return; }
    if (b->hold_len == 7 && prefix(b->hold, 7, LOST)) { b->connected = 0; b->hold_len = 0; return; }
    release(b, 0);
}

static void pump(hermes_device_hm10_t *b) {
    int c;
    while ((c = hermes_uart_port_read_byte(b->port)) >= 0) take(b, (uint8_t)c);
    if (b->hold_len && hermes_timer_millis() - b->hold_ms >= HOLD_MS) /* no notice after all */
        while (b->hold_len) release(b, 1);
}

static void put(hermes_device_hm10_t *b, const char *s) {
    while (*s) hermes_uart_port_write_byte(b->port, (uint8_t)*s++);
}

/* waits for the answer `want` (bytes before it are skipped), up to ms */
static bool answer(hermes_device_hm10_t *b, const char *want, uint32_t ms) {
    uint32_t start = hermes_timer_millis();
    int got = 0;
    while (want[got]) {
        int c = hermes_uart_port_read_byte(b->port);
        if (c < 0) {
            if (hermes_timer_millis() - start >= ms) return false;
            continue;
        }
        got = c == want[got] ? got + 1 : (c == want[0] ? 1 : 0);
    }
    return true;
}

bool hermes_hm10_setup(hermes_device_hm10_t *b, const char *name) {
    if (hermes_hm10_mock_attach) hermes_hm10_mock_attach(b->port);
    bool alive = false;
    for (int i = 0; i < 3 && !alive; i++) {
        put(b, "AT");
        alive = answer(b, "OK", AT_MS);
    }
    if (!alive) return false;
    hermes_timer_delay_ms(HOLD_MS); /* the pause that ends a command */
    put(b, "AT+NAME");
    char n[13];
    int i = 0;
    for (; name[i] && i < 12; i++) n[i] = name[i];
    n[i] = 0;
    put(b, n);
    answer(b, "OK+Set:", AT_MS);
    answer(b, n, AT_MS);
    hermes_timer_delay_ms(HOLD_MS);
    put(b, "AT+NOTI1"); /* "OK+CONN" / "OK+LOST" from now on */
    answer(b, "OK+Set:1", AT_MS);
    return true;
}

bool hermes_hm10_connected(hermes_device_hm10_t *b) {
    if (!b->live) return false;
    pump(b);
    return b->connected;
}

bool hermes_hm10_send(hermes_device_hm10_t *b, const char *s, int n) {
    if (!hermes_hm10_connected(b)) return false;
    for (int i = 0; i < n; i++) hermes_uart_port_write_byte(b->port, (uint8_t)s[i]);
    return true;
}

long hermes_hm10_available(hermes_device_hm10_t *b) {
    if (!b->live) return 0;
    pump(b);
    return (long)((b->head + HERMES_HM10_RX - b->tail) % HERMES_HM10_RX);
}

/* waits 1 ms at a time (under hermes_os the thread sleeps meanwhile) */
long hermes_hm10_read(hermes_device_hm10_t *b, long wait_ms) {
    uint32_t start = hermes_timer_millis();
    for (;;) {
        if (!b->live) return -1;
        pump(b);
        if (b->tail != b->head) {
            uint8_t c = b->rx[b->tail];
            b->tail = (uint16_t)((b->tail + 1) % HERMES_HM10_RX);
            return c;
        }
        if ((long)(hermes_timer_millis() - start) >= wait_ms) return -1;
        hermes_timer_delay_ms(1);
    }
}
