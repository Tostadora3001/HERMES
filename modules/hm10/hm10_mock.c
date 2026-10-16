/* The hm10 module on the host: a virtual HM-10 (modules/hc05/bt_virtual.c)
 * on the UART mock's port; after its set-up a central connects, and the
 * other end echoes back what it gets. */
#include "hermes_drv_uart.h"
#include "../hc05/bt_virtual.h"

static void from_mcu(void *ctx, uint8_t b) { btv_input(ctx, b); }
static int poll(void *ctx) { return btv_output(ctx); }

void hermes_hm10_mock_attach(int port) {
    hermes_uart_mock_device_t dev = { from_mcu, poll, btv_new_hm10() };
    hermes_uart_mock_attach(port, &dev);
}
