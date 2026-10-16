/* SPI driver, host (native) implementation: a mock. Virtual chips, one
 * per chip-select pin (hermes_spi_mock_attach); a transfer to a selected
 * chip goes to its `transfer`, with no chip selected it reads 0xFF (an
 * idle MISO with its pull-up). See hermes_drv_spi.h. */
#include "hermes_drv_spi.h"
#include <pthread.h>
#include <stddef.h>

#define MOCK_CHIPS 4
#define MOCK_CPU_HZ 16000000UL /* answers like an AVR at 16 MHz */

static struct { int cs; hermes_spi_mock_chip_t chip; } g_chips[MOCK_CHIPS];
static int g_nchips;
static int g_selected = -1; /* index in g_chips */
static pthread_mutex_t g_bus = PTHREAD_MUTEX_INITIALIZER;

uint32_t hermes_spi_init(uint32_t hz, uint8_t mode) {
    (void)mode;
    return hermes_spi_clock_for(MOCK_CPU_HZ, hz, NULL);
}

uint8_t hermes_spi_transfer(uint8_t out) {
    if (g_selected < 0 || !g_chips[g_selected].chip.transfer) return 0xFF;
    return g_chips[g_selected].chip.transfer(g_chips[g_selected].chip.ctx, out);
}

void hermes_spi_disable(void) {}

void hermes_spi_cs_setup(int cs) { (void)cs; }

void hermes_spi_select(int cs, int selected) {
    for (int i = 0; i < g_nchips; i++) {
        if (g_chips[i].cs != cs) continue;
        g_selected = selected ? i : -1;
        if (g_chips[i].chip.select) g_chips[i].chip.select(g_chips[i].chip.ctx, selected);
        return;
    }
    if (!selected) g_selected = -1;
}

void hermes_spi_begin(void) { pthread_mutex_lock(&g_bus); }
void hermes_spi_end(void) { pthread_mutex_unlock(&g_bus); }

void hermes_spi_mock_attach(int cs, const hermes_spi_mock_chip_t *chip) {
    for (int i = 0; i < g_nchips; i++)
        if (g_chips[i].cs == cs) { g_chips[i].chip = *chip; return; }
    if (g_nchips < MOCK_CHIPS) {
        g_chips[g_nchips].cs = cs;
        g_chips[g_nchips].chip = *chip;
        g_nchips++;
    }
}
