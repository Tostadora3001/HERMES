/* SPI driver, common part (every platform). See hermes_drv_spi.h. */
#include "hermes_drv_spi.h"

uint32_t hermes_spi_clock_for(uint32_t cpu_hz, uint32_t hz, uint8_t *divider_log2) {
    uint8_t k = 1; /* divider 2^k: 2 .. 128 */
    while (k < 7 && cpu_hz >> k > hz) k++;
    if (divider_log2) *divider_log2 = k;
    return cpu_hz >> k;
}
