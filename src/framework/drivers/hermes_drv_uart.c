/* UART driver, common part (every target). See hermes_drv_uart.h. */
#include "hermes_drv_uart.h"

#if (HERMES_UART_RX_BUFFER & (HERMES_UART_RX_BUFFER - 1)) != 0 || HERMES_UART_RX_BUFFER > 256
#error "HERMES_UART_RX_BUFFER must be a power of two and <= 256"
#endif

void hermes_uart_write_str(const char *s) {
    while (*s) hermes_uart_write_byte((uint8_t)*s++);
}
