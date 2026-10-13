/* UART driver, host (native) implementation: TX to stdout, RX from stdin
 * without blocking. See hermes_drv_uart.h. */
#define _POSIX_C_SOURCE 200809L /* poll, read */
#include "hermes_drv_uart.h"
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

static int g_rx_enabled = 1;

void hermes_uart_init(uint32_t baud) {
    (void)baud;
    g_rx_enabled = 1;
}

void hermes_uart_rx_disable(void) {
    g_rx_enabled = 0;
}

void hermes_uart_write_byte(uint8_t b) {
    fputc(b, stdout);
}

/* poll() also reports "readable" at EOF, so availability can only be
 * known by actually reading: one byte read ahead is kept here. */
static int g_pending = -1;

static void fill_pending(void) {
    if (!g_rx_enabled) { g_pending = -1; return; }
    if (g_pending >= 0) return;
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
    unsigned char b;
    if (read(0, &b, 1) == 1) g_pending = b; /* 0 = EOF: nothing to read */
}

int hermes_uart_available(void) {
    fill_pending();
    return g_pending >= 0 ? 1 : 0;
}

int hermes_uart_read_byte(void) {
    fill_pending();
    int b = g_pending;
    g_pending = -1;
    return b;
}
