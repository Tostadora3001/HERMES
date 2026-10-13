/* GPIO driver, ESP32-C3 implementation. See hermes_drv_gpio.h.
 *
 * Each pin's entry in the board (HERMES_BOARD_PINS) is X(GPIO, n): GPIOn.
 * A pin is routed through the IO_MUX to its "GPIO" function, and its
 * output through the GPIO matrix to the plain output signal (128) with
 * the output enable taken from GPIO_ENABLE_REG. Set/clear registers
 * (W1TS/W1TC) change one pin without a read-modify-write, so no interrupt
 * masking is needed (unlike AVR).
 *
 * Not verified by emulation: Espressif's QEMU models these registers as
 * stubs (they always read 0). */
#include "hermes_drv_gpio.h"
#include "hermes_esp32c3_regs.h"

#define HERMES_GPIO_NUM_(port, bit) (uint8_t)(bit),
static const uint8_t GPIO_NUM[HERMES_GPIO_PIN_COUNT] = { HERMES_BOARD_PINS(HERMES_GPIO_NUM_) };

static void iomux_gpio(uint8_t n, uint32_t extra) {
    uint32_t v = HERMES_REG(IO_MUX_GPIO_REG(n));
    v &= ~(IO_MUX_MCU_SEL_MASK | IO_MUX_FUN_WPU | IO_MUX_FUN_WPD | IO_MUX_FUN_IE);
    v |= (IO_MUX_FUNC_GPIO << IO_MUX_MCU_SEL_SHIFT) | extra;
    HERMES_REG(IO_MUX_GPIO_REG(n)) = v;
}

int hermes_gpio_mode(int pin, hermes_gpio_mode_t mode) {
    if (!hermes_gpio_valid(pin)) return 0;
    uint8_t n = GPIO_NUM[pin];
    if (mode == HERMES_GPIO_OUTPUT) {
        iomux_gpio(n, IO_MUX_FUN_IE); /* input stays on: read() sees the level driven */
        HERMES_REG(GPIO_FUNC_OUT_SEL_CFG_REG(n)) = GPIO_SIG_GPIO_OUT | GPIO_FUNC_OEN_SEL;
        HERMES_REG(GPIO_ENABLE_W1TS_REG) = 1u << n;
    } else {
        HERMES_REG(GPIO_ENABLE_W1TC_REG) = 1u << n;
        iomux_gpio(n, IO_MUX_FUN_IE | (mode == HERMES_GPIO_INPUT_PULLUP ? IO_MUX_FUN_WPU : 0));
    }
    return 1;
}

void hermes_gpio_write(int pin, bool high) {
    if (!hermes_gpio_valid(pin)) return;
    HERMES_REG(high ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG) = 1u << GPIO_NUM[pin];
}

bool hermes_gpio_read(int pin) {
    if (!hermes_gpio_valid(pin)) return false;
    return (HERMES_REG(GPIO_IN_REG) >> GPIO_NUM[pin]) & 1u;
}
