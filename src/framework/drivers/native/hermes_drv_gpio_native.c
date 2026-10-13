/* GPIO driver, host (native) implementation: a mock with the board's
 * pins. See hermes_drv_gpio.h. */
#include "hermes_drv_gpio.h"
#include <stdio.h>

static hermes_gpio_mode_t g_mode[HERMES_GPIO_PIN_COUNT];
static bool g_level[HERMES_GPIO_PIN_COUNT];

int hermes_gpio_mode(int pin, hermes_gpio_mode_t mode) {
    if (!hermes_gpio_valid(pin)) return 0;
    g_mode[pin] = mode;
    if (mode == HERMES_GPIO_INPUT_PULLUP) g_level[pin] = true; /* pulled up until driven low */
    return 1;
}

void hermes_gpio_write(int pin, bool high) {
    if (!hermes_gpio_valid(pin)) return;
    g_level[pin] = high;
    printf("[gpio mock] pin %d -> %s\n", pin, high ? "HIGH" : "LOW");
}

bool hermes_gpio_read(int pin) {
    return hermes_gpio_valid(pin) ? g_level[pin] : false;
}

void hermes_gpio_mock_set_input(int pin, bool high) {
    if (hermes_gpio_valid(pin)) g_level[pin] = high;
}
