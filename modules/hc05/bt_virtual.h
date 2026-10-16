#ifndef BT_VIRTUAL_H
#define BT_VIRTUAL_H

/* Virtual Bluetooth modules, test only: an HC-05 and an HM-10, each
 * connected to a remote device that echoes back what it gets. Used by the
 * modules' mocks on the host (on a port of the UART mock) and by the AVR
 * test bench (scripts/simavr_bench --hc05 N / --hm10 N). Not part of the
 * framework. Shared by the hc05 and hm10 modules (this file lives in
 * modules/hc05; modules/hm10 compiles it from there). */

#include <stdint.h>

typedef struct btv btv_t;

btv_t *btv_new_hc05(void); /* paired and connected from the start */
btv_t *btv_new_hm10(void); /* AT commands until AT+NOTI1, then a central
                              connects ("OK+CONN") */
void btv_input(btv_t *v, uint8_t b); /* a byte from the MCU */
int btv_output(btv_t *v);            /* its next byte for the MCU, -1: none */
int btv_connected(btv_t *v);         /* the HC-05's STATE pin */

#endif
