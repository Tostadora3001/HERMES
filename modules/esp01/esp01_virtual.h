#ifndef ESP01_VIRTUAL_H
#define ESP01_VIRTUAL_H

/* A virtual ESP-01 (ESP8266 with Espressif's AT firmware), test only: the
 * subset of AT commands the esp01 module uses, its answers and its
 * unsolicited messages (+IPD, n,CONNECT, n,CLOSED), with its connections
 * carried out by the host's own sockets. Used by the module's mock on the
 * host (on a serial port of the UART mock) and by the AVR test bench
 * (scripts/simavr_bench --esp01 N: on USART N). Not part of the framework. */

#include <stdint.h>

typedef struct esp01v esp01v_t;

esp01v_t *esp01v_new(void);
void esp01v_input(esp01v_t *v, uint8_t b); /* a byte from the MCU */
int esp01v_output(esp01v_t *v);            /* its next byte for the MCU, -1: none */

#endif
