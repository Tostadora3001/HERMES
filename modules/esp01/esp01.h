#ifndef ESP01_H
#define ESP01_H

/* `import esp01`: an ESP-01 (ESP8266 with Espressif's AT firmware) on a
 * serial port as a hermes_net interface (asked by the user, 2026-10-16:
 * phase 3 of the communications plan; the TCP/IP stack and the Wi-Fi are
 * the module's):
 *
 *   device serial = uart_port(1, 115200)          the ESP-01's port (its
 *                                                 factory speed: 115200)
 *   device wifi = esp01(serial, "ssid", "clave")  joins that network
 *   device c = wifi.tcp("192.168.1.10", 80)       then hermes_net's sockets
 *
 * A module that doesn't answer AT is a fatal error with its message; a
 * network it can't join is not (a warning: its sockets then fail, as with
 * a network that goes down). Five links (the firmware's). On the host the
 * module is a virtual one (esp01_virtual.c) on the UART mock's port, whose
 * connections are the PC's own sockets. See docs/raw/hermes_net.en.md. */

#include "hermes_net.h"
#include "hermes_uart.h"

void *hermes_esp01_init(int port, const char *ssid, const char *pass); /* NULL: no answer */
extern const hermes_net_ops_t hermes_esp01_ops;

static inline hermes_device_net_handle_t esp01(hermes_device_uart_handle_t serial, const char *ssid, const char *pass) {
    int port = (int)device_uart_port(serial);
    void *nif = hermes_esp01_init(port, ssid, pass);
    if (!nif) {
        HERMES_DIAG("esp01: no ESP-01 answers on serial port %d\n", port);
        hermes_fatal();
    }
    return hermes_net_new(&hermes_esp01_ops, nif, "esp01");
}

#endif
