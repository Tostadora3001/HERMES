/* The w5500 module on the host: a virtual W5500 (w5500_virtual.c) put on
 * the SPI driver's mock at the module's chip-select pin -- the driver
 * then speaks the chip's real SPI frames, and the virtual chip's sockets
 * are the PC's own. */
#include "hermes_drv_spi.h"
#include "w5500_virtual.h"

static w5500v_t *g_virtual;

static void sel(void *ctx, int selected) { w5500v_select(ctx, selected); }
static uint8_t xfer(void *ctx, uint8_t in) { return w5500v_transfer(ctx, in); }

void hermes_w5500_mock_attach(int cs) {
    if (!g_virtual) g_virtual = w5500v_new();
    hermes_spi_mock_chip_t chip = { sel, xfer, g_virtual };
    hermes_spi_mock_attach(cs, &chip);
}
