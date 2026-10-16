/* SPI driver, AVR implementation: the SPI peripheral as master, by
 * polling (same registers on the ATmega328P and the ATmega2560). See
 * hermes_drv_spi.h. */
#include "hermes_drv_spi.h"
#include "hermes_drv_gpio.h"
#include "hermes_board.h"
#include <avr/io.h>

/* weak: the OS's preemption, if linked (a transaction isn't split) */
extern void hermes_preempt_off(void) __attribute__((weak));
extern void hermes_preempt_on(void) __attribute__((weak));

uint32_t hermes_spi_init(uint32_t hz, uint8_t mode) {
    uint8_t k;
    uint32_t got = hermes_spi_clock_for(F_CPU, hz, &k);
    /* SS must be an output while master: as an input, a low level on it
     * would turn the peripheral into a slave */
    hermes_gpio_mode(HERMES_BOARD_SPI_SS_PIN, HERMES_GPIO_OUTPUT);
    hermes_gpio_write(HERMES_BOARD_SPI_SS_PIN, true);
    hermes_gpio_mode(HERMES_BOARD_SPI_SCK_PIN, HERMES_GPIO_OUTPUT);
    hermes_gpio_mode(HERMES_BOARD_SPI_MOSI_PIN, HERMES_GPIO_OUTPUT);
    hermes_gpio_mode(HERMES_BOARD_SPI_MISO_PIN, HERMES_GPIO_INPUT);
    /* divider 2^k: SPR1:0 = (k - 1) / 2; SPI2X for the odd ones below 128 */
    uint8_t spr = (uint8_t)((k - 1) >> 1);
    SPCR = (uint8_t)((1 << SPE) | (1 << MSTR) | ((mode & 3) << CPHA) | spr);
    SPSR = (uint8_t)((k & 1) && k < 7 ? (1 << SPI2X) : 0);
    return got;
}

uint8_t hermes_spi_transfer(uint8_t out) {
    SPDR = out;
    while (!(SPSR & (1 << SPIF))) {}
    return SPDR;
}

void hermes_spi_disable(void) {
    SPCR = 0;
    hermes_gpio_mode(HERMES_BOARD_SPI_SCK_PIN, HERMES_GPIO_INPUT);
    hermes_gpio_mode(HERMES_BOARD_SPI_MOSI_PIN, HERMES_GPIO_INPUT);
}

void hermes_spi_cs_setup(int cs) {
    hermes_gpio_write(cs, true);
    hermes_gpio_mode(cs, HERMES_GPIO_OUTPUT);
}

void hermes_spi_select(int cs, int selected) {
    hermes_gpio_write(cs, !selected);
}

void hermes_spi_begin(void) { if (hermes_preempt_off) hermes_preempt_off(); }
void hermes_spi_end(void) { if (hermes_preempt_on) hermes_preempt_on(); }

/* host-only (the virtual chips) */
void hermes_spi_mock_attach(int cs, const hermes_spi_mock_chip_t *chip) { (void)cs; (void)chip; }
