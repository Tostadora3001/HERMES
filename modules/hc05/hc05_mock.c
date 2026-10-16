/* The hc05 module on the host: a virtual HC-05 (bt_virtual.c) on the UART
 * mock's port, paired to a device that echoes; its STATE pin (if wired)
 * reads high through the GPIO mock. */
#include "hermes_drv_uart.h"
#include "hermes_drv_gpio.h"
#include "bt_virtual.h"

static void from_mcu(void *ctx, uint8_t b) { btv_input(ctx, b); }
static int poll(void *ctx) { return btv_output(ctx); }

void hermes_hc05_mock_attach(int port, int state_pin) {
    btv_t *v = btv_new_hc05();
    hermes_uart_mock_device_t dev = { from_mcu, poll, v };
    hermes_uart_mock_attach(port, &dev);
    if (state_pin >= 0) hermes_gpio_mock_set_input(state_pin, btv_connected(v));
}
