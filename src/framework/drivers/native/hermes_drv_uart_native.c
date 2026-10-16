/* UART driver, host (native) implementation. See hermes_drv_uart.h.
 * Port 0: TX to stdout, RX from stdin without blocking. Ports 1-3: wires
 * in memory -- 1 <-> 2 crossed, 3 a loopback -- with optional faults
 * (hermes_uart_mock_faults), as scripts/simavr_bench wires a Mega's
 * USARTs, so the same program behaves the same on both. */
#define _POSIX_C_SOURCE 200809L /* poll, read */
#include "hermes_drv_uart.h"
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define HERMES_UART_HOST_PORTS 4
#define WIRE_BUF 256 /* bytes a wire holds before it drops them */

static int g_rx_enabled[HERMES_UART_HOST_PORTS] = { 1, 1, 1, 1 };

/* the wires: each port's receive queue (threads on several cores may use
 * them at once: a mutex of their own -- the driver doesn't depend on the
 * framework's lock) */
static pthread_mutex_t g_wire_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    unsigned char buf[WIRE_BUF];
    unsigned head, tail; /* tail == head: empty */
} g_rx[HERMES_UART_HOST_PORTS];
static int g_drop_every, g_flip_every;
static unsigned long g_wire_bytes;

/* a virtual device on a port (hermes_uart_mock_attach) */
static hermes_uart_mock_device_t g_dev[HERMES_UART_HOST_PORTS];

void hermes_uart_mock_attach(int port, const hermes_uart_mock_device_t *dev) {
    if (port > 0 && port < HERMES_UART_HOST_PORTS) g_dev[port] = *dev;
}

/* what the device has for the program, into the port's queue */
static void dev_poll(int port) {
    if (!g_dev[port].poll) return;
    for (;;) {
        unsigned next = (g_rx[port].head + 1) % WIRE_BUF;
        if (next == g_rx[port].tail) break; /* full: the rest waits in the device (asked only with room) */
        int b = g_dev[port].poll(g_dev[port].ctx);
        if (b < 0) break;
        g_rx[port].buf[g_rx[port].head] = (unsigned char)b;
        g_rx[port].head = next;
    }
}

static int peer(int port) {
    return port == 1 ? 2 : port == 2 ? 1 : port; /* 3: itself */
}

int hermes_uart_port_exists(int port) {
    return port >= 0 && port < HERMES_UART_HOST_PORTS;
}

void hermes_uart_port_init(int port, uint32_t baud) {
    (void)baud;
    if (hermes_uart_port_exists(port)) g_rx_enabled[port] = 1;
}

void hermes_uart_port_rx_disable(int port) {
    if (!hermes_uart_port_exists(port)) return;
    g_rx_enabled[port] = 0;
    pthread_mutex_lock(&g_wire_lock);
    g_rx[port].tail = g_rx[port].head; /* discard what was pending */
    pthread_mutex_unlock(&g_wire_lock);
}

void hermes_uart_mock_faults(int drop_every, int flip_every) {
    pthread_mutex_lock(&g_wire_lock);
    g_drop_every = drop_every > 0 ? drop_every : 0;
    g_flip_every = flip_every > 0 ? flip_every : 0;
    g_wire_bytes = 0;
    pthread_mutex_unlock(&g_wire_lock);
}

void hermes_uart_port_write_byte(int port, uint8_t b) {
    if (port == 0) { fputc(b, stdout); return; }
    if (!hermes_uart_port_exists(port)) return;
    if (g_dev[port].from_mcu) { g_dev[port].from_mcu(g_dev[port].ctx, b); return; }
    int to = peer(port);
    pthread_mutex_lock(&g_wire_lock);
    unsigned long k = ++g_wire_bytes;
    int lost = g_drop_every && k % (unsigned long)g_drop_every == 0;
    if (!lost && g_flip_every && k % (unsigned long)g_flip_every == 0) b ^= 0x10;
    unsigned next = (g_rx[to].head + 1) % WIRE_BUF;
    if (!lost && g_rx_enabled[to] && next != g_rx[to].tail) {
        g_rx[to].buf[g_rx[to].head] = b;
        g_rx[to].head = next;
    }
    pthread_mutex_unlock(&g_wire_lock);
}

/* Port 0: poll() also reports "readable" at EOF, so availability can only
 * be known by actually reading: one byte read ahead is kept here. */
static int g_pending = -1;

static void fill_pending(void) {
    if (!g_rx_enabled[0]) { g_pending = -1; return; }
    if (g_pending >= 0) return;
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
    unsigned char b;
    if (read(0, &b, 1) == 1) g_pending = b; /* 0 = EOF: nothing to read */
}

int hermes_uart_port_available(int port) {
    if (port == 0) {
        fill_pending();
        return g_pending >= 0 ? 1 : 0;
    }
    if (!hermes_uart_port_exists(port)) return 0;
    pthread_mutex_lock(&g_wire_lock);
    dev_poll(port);
    int n = (int)((g_rx[port].head + WIRE_BUF - g_rx[port].tail) % WIRE_BUF);
    pthread_mutex_unlock(&g_wire_lock);
    return n;
}

int hermes_uart_port_read_byte(int port) {
    if (port == 0) {
        fill_pending();
        int b = g_pending;
        g_pending = -1;
        return b;
    }
    if (!hermes_uart_port_exists(port)) return -1;
    int b = -1;
    pthread_mutex_lock(&g_wire_lock);
    dev_poll(port);
    if (g_rx[port].tail != g_rx[port].head) {
        b = g_rx[port].buf[g_rx[port].tail];
        g_rx[port].tail = (g_rx[port].tail + 1) % WIRE_BUF;
    }
    pthread_mutex_unlock(&g_wire_lock);
    return b;
}
