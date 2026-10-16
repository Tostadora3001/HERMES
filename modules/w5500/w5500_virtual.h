#ifndef W5500_VIRTUAL_H
#define W5500_VIRTUAL_H

/* A virtual W5500 (test only): the chip's SPI frames, its registers and
 * socket buffers, and its eight sockets carried out with the host's own
 * sockets (POSIX) -- a TCP CONNECT to 127.0.0.1:7 really connects there.
 * Used twice, so both test the same thing: the w5500 module's mock on the
 * host (w5500_mock.c, through the SPI driver's virtual chips) and the AVR
 * test bench (scripts/simavr_bench, --w5500), where it is the chip at the
 * other end of the simulated SPI bus. Not part of the framework. */

#include <stdint.h>

typedef struct w5500v w5500v_t;

w5500v_t *w5500v_new(void);
void w5500v_select(w5500v_t *w, int selected); /* CS low (1) / high (0) */
uint8_t w5500v_transfer(w5500v_t *w, uint8_t in); /* one SPI byte */

#endif
