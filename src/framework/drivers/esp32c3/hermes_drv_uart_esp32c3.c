/* UART driver, ESP32-C3 implementation: UART0 (GPIO21 TX / GPIO20 RX,
 * the USB-serial bridge of the DevKits). See hermes_drv_uart.h.
 *
 * The ROM leaves UART0 configured at 115200 8N1 (it prints its boot
 * banner there), so with no explicit uart(baud) nothing is reconfigured:
 * HERMES_UART_DEFAULT_BAUD is 115200 on this platform.
 *
 * Unlike AVR, no interrupt: the UART has a 128-byte hardware RX FIFO,
 * which is the receive buffer (HERMES_UART_RX_BUFFER isn't used here).
 * TX waits for room in the 128-byte TX FIFO, without yielding (a print()
 * line is never interleaved with another thread's). */
#include "hermes_drv_uart.h"
#include "hermes_esp32c3_regs.h"

static uint8_t g_rx_enabled = 1;

static uint32_t rx_count(void) { return HERMES_REG(UART_STATUS_REG) & 0x3FFu; }
static uint32_t tx_count(void) { return (HERMES_REG(UART_STATUS_REG) >> 16) & 0x3FFu; }

static void rx_flush(void) {
    HERMES_REG(UART_CONF0_REG) |= UART_RXFIFO_RST;
    HERMES_REG(UART_CONF0_REG) &= ~UART_RXFIFO_RST;
}

void hermes_uart_init(uint32_t baud) {
    if (baud == 0) baud = HERMES_UART_DEFAULT_BAUD;
    while (tx_count() > 0) {} /* let what is pending leave at the old rate */
    /* clock: XTAL (40 MHz), undivided; divider in 1/16ths of a cycle */
    uint32_t clk = HERMES_REG(UART_CLK_CONF_REG);
    clk &= ~((3u << UART_SCLK_SEL_SHIFT) | UART_SCLK_DIV_NUM_MASK);
    clk |= UART_SCLK_SEL_XTAL << UART_SCLK_SEL_SHIFT;
    HERMES_REG(UART_CLK_CONF_REG) = clk;
    uint32_t div16 = (HERMES_ESP32C3_XTAL_HZ * 16u + baud / 2) / baud;
    HERMES_REG(UART_CLKDIV_REG) = ((div16 & 0xFu) << 20) | (div16 >> 4);
    /* the new configuration reaches the UART core when REG_UPDATE is set;
     * the hardware clears it when done (bounded wait: not every model of
     * the chip implements it) */
    HERMES_REG(UART_ID_REG) |= UART_REG_UPDATE;
    for (int i = 0; i < 1000 && (HERMES_REG(UART_ID_REG) & UART_REG_UPDATE); i++) {}
    g_rx_enabled = 1;
}

/* "Room?" and the write with interrupts off: under hermes_os the tick may
 * switch threads in between (preemption), and other threads' bytes would
 * take the room first. The wait itself keeps interrupts on. */
void hermes_uart_write_byte(uint8_t b) {
    for (;;) {
        unsigned long mie;
        __asm__ __volatile__("csrrci %0, mstatus, 8" : "=r"(mie) :: "memory");
        int room = tx_count() < UART_FIFO_SIZE - 1;
        if (room) HERMES_REG(UART_FIFO_REG) = b;
        __asm__ __volatile__("csrs mstatus, %0" :: "r"(mie & 8) : "memory");
        if (room) return;
    }
}

void hermes_uart_rx_disable(void) {
    g_rx_enabled = 0;
    rx_flush(); /* discard what was pending */
}

int hermes_uart_available(void) {
    return g_rx_enabled ? (int)rx_count() : 0;
}

int hermes_uart_read_byte(void) {
    if (!g_rx_enabled) return -1;
    unsigned long mie; /* "any?" and the read together (see write_byte) */
    __asm__ __volatile__("csrrci %0, mstatus, 8" : "=r"(mie) :: "memory");
    int b = rx_count() ? (int)(HERMES_REG(UART_FIFO_REG) & 0xFFu) : -1;
    __asm__ __volatile__("csrs mstatus, %0" :: "r"(mie & 8) : "memory");
    return b;
}

/* Overrides hermes_stdlib.h's weak no-op: print() -> serial port. */
void hermes_putchar(char c) {
    hermes_uart_write_byte((uint8_t)c);
}
