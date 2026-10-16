#ifndef HERMES_DRV_SPI_H
#define HERMES_DRV_SPI_H

/* Core driver: the SPI bus, as master (asked by the user, 2026-10-16:
 * phase 2 of the communications plan -- the W5500 Ethernet chip speaks
 * SPI). Pins: the board's HERMES_BOARD_SPI_SCK/MOSI/MISO/SS_PIN (Uno: D13
 * / D11 / D12 / D10; Mega: D52 / D51 / D50 / D53). Each chip on the bus
 * has its own chip-select pin (any digital pin), driven by
 * hermes_spi_select: low while the chip is selected.
 *
 * Common interface; one implementation per platform:
 *   hermes_drv_spi.c               common part: the clock divider arithmetic
 *   avr/hermes_drv_spi_avr.c       the SPI peripheral, by polling
 *   native/hermes_drv_spi_native.c a mock: virtual chips, one per
 *                                  chip-select pin (hermes_spi_mock_attach)
 * No ESP32-C3 implementation yet: hepyc reports it.
 *
 * Modes 0-3 (CPOL, CPHA), most significant bit first. A transfer
 * clocks one byte out and one in, waiting for the hardware without
 * yielding (a byte takes 1 us at 8 MHz). A transaction (chip selected,
 * bytes, chip released) runs between hermes_spi_begin() and
 * hermes_spi_end(): on bare metal the OS doesn't take the core meanwhile
 * (another thread could select another chip in the middle); on the host,
 * a mutex. */

#include <stdint.h>

/* Configures the bus (master; SCK, MOSI and SS outputs, MISO input).
 * Returns the clock actually used: the fastest the hardware has that
 * isn't above `hz`. */
uint32_t hermes_spi_init(uint32_t hz, uint8_t mode);

/* Clocks `out` out and returns the byte clocked in. */
uint8_t hermes_spi_transfer(uint8_t out);

/* The bus off (SCK/MOSI back to inputs). */
void hermes_spi_disable(void);

/* Chip-select pin `cs`: an output, high (released) -- then low while
 * `selected`. AVR: through the GPIO driver; host: tells the virtual chip. */
void hermes_spi_cs_setup(int cs);
void hermes_spi_select(int cs, int selected);

/* A transaction's bounds (see above). */
void hermes_spi_begin(void);
void hermes_spi_end(void);

/* Common part: the clock that `hz` gives on a chip whose SPI divides
 * `cpu_hz` by 2, 4, 8 ... 128 (AVR; and the mock, to give the same
 * answers): the fastest not above `hz` (the slowest if none). */
uint32_t hermes_spi_clock_for(uint32_t cpu_hz, uint32_t hz, uint8_t *divider_log2);

/* Host only: a virtual chip on chip-select pin `cs`. `select` is called
 * when the pin goes low (1) and high (0); `transfer` for each byte while
 * selected. Up to 4 chips. */
typedef struct {
    void (*select)(void *ctx, int selected);
    uint8_t (*transfer)(void *ctx, uint8_t in);
    void *ctx;
} hermes_spi_mock_chip_t;
void hermes_spi_mock_attach(int cs, const hermes_spi_mock_chip_t *chip);

#endif
