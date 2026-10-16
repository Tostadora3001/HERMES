#ifndef W5500_H
#define W5500_H

/* `import w5500`: the WIZnet W5500 Ethernet chip (SPI) as a hermes_net
 * interface (asked by the user, 2026-10-16: phase 2 of the communications
 * plan; the TCP/IP stack is the chip's). Needs a chip of an SPI bus:
 *
 *   device bus = spi(8000000, 0)
 *   device chip = bus.at(10)                  its chip select (the Arduino
 *                                             Ethernet shield: D10)
 *   device eth = w5500(chip, "192.168.1.50")  its address (mask
 *                                             255.255.255.0, gateway and
 *                                             DNS a.b.c.1)
 *   device eth = w5500(chip, "0.0.0.0")       or none yet, and then
 *   eth.dhcp()                                one from DHCP (with mask,
 *                                             gateway and DNS; renewed at
 *                                             half the lease)
 *   device c = eth.tcp("192.168.1.10", 80)    then the sockets of hermes_net
 *
 * A W5500 that doesn't answer (VERSIONR != 4) is a fatal error with its
 * message; no address from DHCP is not (dhcp() returns False: a network
 * can be down). MAC: 02:00:00:00:00:<last byte of the IP> (a locally
 * administered one; with "0.0.0.0", its last three bytes from the clock at
 * start, so that two boards don't share one). Eight sockets, shared by connections, UDP, a server's listening
 * socket and, for a moment, DHCP and DNS. On the host, the chip is a virtual one
 * (w5500_virtual.c) whose sockets are the PC's own: the same program
 * speaks the same SPI frames on the PC as on a board. See
 * docs/raw/hermes_net.en.md. */

#include "hermes_net.h"
#include "hermes_spi.h"

void *hermes_w5500_init(int cs, const uint8_t ip[4]); /* NULL: no chip */
extern const hermes_net_ops_t hermes_w5500_ops;

static inline hermes_device_net_handle_t w5500(hermes_device_spi_dev_handle_t chip, const char *ip) {
    int cs = hermes_spi_dev_cs(chip, HERMES_PSTR("w5500"));
    uint8_t a[4];
    if (!hermes_net_parse_ip(ip, a)) hermes_net_bad_ip(HERMES_PSTR("w5500"), ip);
    void *nif = hermes_w5500_init(cs, a);
    if (!nif) {
        HERMES_DIAG("w5500: no W5500 answers on chip select pin %d\n", cs);
        hermes_fatal();
    }
    return hermes_net_new(&hermes_w5500_ops, nif, "w5500");
}

#endif
