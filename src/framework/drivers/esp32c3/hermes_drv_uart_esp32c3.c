/* UART driver, ESP32-C3 implementation: UART0 (GPIO21 TX / GPIO20 RX, the
 * USB-serial bridge of the DevKits) and UART1 (routed by the GPIO matrix
 * to the board's pins: HERMES_BOARD_UART_PINS). See hermes_drv_uart.h.
 *
 * The ROM leaves UART0 configured at 115200 8N1 (it prints its boot
 * banner there), so with no explicit uart(baud) nothing is reconfigured:
 * HERMES_UART_DEFAULT_BAUD is 115200 on this platform. UART1 is off at
 * boot: its first init turns on its clock, releases its reset and routes
 * its signals to the pins.
 *
 * Unlike AVR, no interrupt: each UART has a 128-byte hardware RX FIFO,
 * which is the receive buffer (HERMES_UART_RX_BUFFER isn't used here).
 * TX waits for room in the 128-byte TX FIFO, without yielding (a print()
 * line is never interleaved with another thread's). */
#include "hermes_drv_uart.h"
#include "hermes_esp32c3_regs.h"
#include "hermes_board.h"

#define HERMES_UART_HW_PORTS 2

static uint8_t g_rx_enabled[HERMES_UART_HW_PORTS] = { 1, 0 };
static uint8_t g_on[HERMES_UART_HW_PORTS] = { 1, 0 }; /* UART0: the ROM's */

#define REG(port, reg) HERMES_REG(UART_PORT_REG(port, reg))

static uint32_t rx_count(int port) { return REG(port, UART_STATUS_REG) & 0x3FFu; }
static uint32_t tx_count(int port) { return (REG(port, UART_STATUS_REG) >> 16) & 0x3FFu; }

static void rx_flush(int port) {
    REG(port, UART_CONF0_REG) |= UART_RXFIFO_RST;
    REG(port, UART_CONF0_REG) &= ~UART_RXFIFO_RST;
}

int hermes_uart_port_exists(int port) {
    return port >= 0 && port < HERMES_UART_HW_PORTS;
}

/* the pins of a port (board table) */
#define HERMES_UART_PIN_RX(p, rx, tx) if (port == (p)) return (rx);
#define HERMES_UART_PIN_TX(p, rx, tx) if (port == (p)) return (tx);
static int pin_rx(int port) { HERMES_BOARD_UART_PINS(HERMES_UART_PIN_RX) return -1; }
static int pin_tx(int port) { HERMES_BOARD_UART_PINS(HERMES_UART_PIN_TX) return -1; }

/* UART1's first init: clock on, reset released, signals to the pins. */
static void uart1_power_and_route(void) {
    HERMES_REG(SYSTEM_PERIP_CLK_EN0_REG) |= SYSTEM_UART1_BIT;
    HERMES_REG(SYSTEM_PERIP_RST_EN0_REG) &= ~SYSTEM_UART1_BIT;
    int rx = pin_rx(1), tx = pin_tx(1);
    if (tx >= 0) {
        uint32_t v = HERMES_REG(IO_MUX_GPIO_REG(tx));
        v &= ~(IO_MUX_MCU_SEL_MASK | IO_MUX_FUN_WPU | IO_MUX_FUN_WPD);
        HERMES_REG(IO_MUX_GPIO_REG(tx)) = v | (IO_MUX_FUNC_GPIO << IO_MUX_MCU_SEL_SHIFT);
        HERMES_REG(GPIO_FUNC_OUT_SEL_CFG_REG(tx)) = GPIO_SIG_U1TXD_OUT; /* OE from the UART */
        HERMES_REG(GPIO_ENABLE_W1TS_REG) = 1u << tx;
    }
    if (rx >= 0) {
        uint32_t v = HERMES_REG(IO_MUX_GPIO_REG(rx));
        v &= ~(IO_MUX_MCU_SEL_MASK | IO_MUX_FUN_WPD);
        /* input on, pull-up: an idle line reads 1 (no false start bits) */
        HERMES_REG(IO_MUX_GPIO_REG(rx)) = v | (IO_MUX_FUNC_GPIO << IO_MUX_MCU_SEL_SHIFT) | IO_MUX_FUN_IE | IO_MUX_FUN_WPU;
        HERMES_REG(GPIO_FUNC_IN_SEL_CFG_REG(GPIO_SIG_U1RXD_IN)) = (uint32_t)rx | GPIO_SIG_IN_SEL;
    }
}

void hermes_uart_port_init(int port, uint32_t baud) {
    if (!hermes_uart_port_exists(port)) return;
    if (baud == 0) baud = HERMES_UART_DEFAULT_BAUD;
    if (!g_on[port]) {
        uart1_power_and_route();
        g_on[port] = 1;
    }
    while (tx_count(port) > 0) {} /* let what is pending leave at the old rate */
    /* clock: XTAL (40 MHz), undivided; divider in 1/16ths of a cycle */
    uint32_t clk = REG(port, UART_CLK_CONF_REG);
    clk &= ~((3u << UART_SCLK_SEL_SHIFT) | UART_SCLK_DIV_NUM_MASK);
    clk |= UART_SCLK_SEL_XTAL << UART_SCLK_SEL_SHIFT;
    REG(port, UART_CLK_CONF_REG) = clk;
    uint32_t div16 = (HERMES_ESP32C3_XTAL_HZ * 16u + baud / 2) / baud;
    REG(port, UART_CLKDIV_REG) = ((div16 & 0xFu) << 20) | (div16 >> 4);
    /* the new configuration reaches the UART core when REG_UPDATE is set;
     * the hardware clears it when done (bounded wait: not every model of
     * the chip implements it) */
    REG(port, UART_ID_REG) |= UART_REG_UPDATE;
    for (int i = 0; i < 1000 && (REG(port, UART_ID_REG) & UART_REG_UPDATE); i++) {}
    g_rx_enabled[port] = 1;
}

/* "Room?" and the write with interrupts off: under hermes_os the tick may
 * switch threads in between (preemption), and other threads' bytes would
 * take the room first. The wait itself keeps interrupts on. */
void hermes_uart_port_write_byte(int port, uint8_t b) {
    if (!hermes_uart_port_exists(port)) return;
    if (!g_on[port]) hermes_uart_port_init(port, HERMES_UART_DEFAULT_BAUD);
    for (;;) {
        unsigned long mie;
        __asm__ __volatile__("csrrci %0, mstatus, 8" : "=r"(mie) :: "memory");
        int room = tx_count(port) < UART_FIFO_SIZE - 1;
        if (room) REG(port, UART_FIFO_REG) = b;
        __asm__ __volatile__("csrs mstatus, %0" :: "r"(mie & 8) : "memory");
        if (room) return;
    }
}

void hermes_uart_port_rx_disable(int port) {
    if (!hermes_uart_port_exists(port)) return;
    g_rx_enabled[port] = 0;
    if (g_on[port]) rx_flush(port); /* discard what was pending */
}

int hermes_uart_port_available(int port) {
    if (!hermes_uart_port_exists(port) || !g_rx_enabled[port]) return 0;
    return (int)rx_count(port);
}

int hermes_uart_port_read_byte(int port) {
    if (!hermes_uart_port_exists(port) || !g_rx_enabled[port]) return -1;
    unsigned long mie; /* "any?" and the read together (see write_byte) */
    __asm__ __volatile__("csrrci %0, mstatus, 8" : "=r"(mie) :: "memory");
    int b = rx_count(port) ? (int)(REG(port, UART_FIFO_REG) & 0xFFu) : -1;
    __asm__ __volatile__("csrs mstatus, %0" :: "r"(mie & 8) : "memory");
    return b;
}

/* host-only (the wires in memory) */
void hermes_uart_mock_faults(int drop_every, int flip_every) { (void)drop_every; (void)flip_every; }
void hermes_uart_mock_attach(int port, const hermes_uart_mock_device_t *dev) { (void)port; (void)dev; }

/* Overrides hermes_stdlib.h's weak no-op: print() -> serial port 0. */
void hermes_putchar(char c) {
    hermes_uart_port_write_byte(0, (uint8_t)c);
}
