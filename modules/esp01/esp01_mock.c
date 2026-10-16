/* The esp01 module on the host: a virtual ESP-01 (esp01_virtual.c) put on
 * the UART mock's port -- the driver then speaks real AT commands, and the
 * virtual module's connections are the PC's own sockets. */
#include "hermes_drv_uart.h"
#include "esp01_virtual.h"

static void from_mcu(void *ctx, uint8_t b) { esp01v_input(ctx, b); }
static int poll(void *ctx) { return esp01v_output(ctx); }

void hermes_esp01_mock_attach(int port) {
    static esp01v_t *v;
    if (!v) v = esp01v_new();
    hermes_uart_mock_device_t dev = { from_mcu, poll, v };
    hermes_uart_mock_attach(port, &dev);
}
