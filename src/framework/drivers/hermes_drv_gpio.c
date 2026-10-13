/* GPIO driver, common part (every target). See hermes_drv_gpio.h. */
#include "hermes_drv_gpio.h"

int hermes_gpio_valid(int pin) {
    return pin >= 0 && pin < HERMES_GPIO_PIN_COUNT && !HERMES_BOARD_PIN_RESERVED(pin);
}
