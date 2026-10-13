#ifndef HERMES_DIAG_H
#define HERMES_DIAG_H

/* HERMES_DIAG(fmt, ...): the framework's error/diagnostic messages
 * (printf-style; used right before hermes_fatal()).
 *
 *   host: fprintf(stderr, ...).
 *   bare metal (AVR, RISC-V microcontrollers; hermes_platform.h) with the
 *        UART linked (`import hermes_uart`; hepyc then passes
 *        -DHERMES_DIAG_UART): out through the UART (hermes_putchar) with a
 *        tiny formatter of our own (hermes_diag.c) -- avr-libc's printf
 *        engine alone costs ~1.5 KB of flash.
 *        Supported: %s %d %ld %u %lu %zu %p %c %% -- and %S, a string
 *        given with HERMES_PSTR.
 *        AVR: the format string goes to flash (PSTR): as a plain literal
 *        avr-libc would copy it to RAM at startup, and all the messages
 *        together would eat a good part of the 2 KB. Elsewhere constant
 *        strings are read where they are: no PSTR needed.
 *   bare metal without the UART: nothing (no way out; the program doesn't
 *        pay for the messages at all). */

/* A text passed as an ARGUMENT of a message (an operation's name...) is
 * written HERMES_PSTR("run") and printed with "%" HERMES_PS: on AVR it then
 * stays in flash too (a plain literal would be copied to RAM: that cost
 * ~130 B of RAM in the first version), and without the UART it doesn't
 * exist at all. */
#include "hermes_platform.h"

#if HERMES_BARE_METAL
#if defined(HERMES_DIAG_UART)
#if defined(__AVR__)
#include <avr/pgmspace.h>
#define HERMES_FLASH_STR(s) PSTR(s)
#else
#define HERMES_FLASH_STR(s) (s)
#endif
void hermes_diag_P(const char *fmt_P, ...);
#define HERMES_DIAG(fmt, ...) hermes_diag_P(HERMES_FLASH_STR(fmt), ##__VA_ARGS__)
#define HERMES_PSTR(s) HERMES_FLASH_STR(s)
#define HERMES_PS "S"
#else
#define HERMES_DIAG(...) ((void)0)
#define HERMES_PSTR(s) ((const char *)0)
#define HERMES_PS "S"
#endif
#else
#include <stdio.h>
#define HERMES_DIAG(...) fprintf(stderr, __VA_ARGS__)
#define HERMES_PSTR(s) (s)
#define HERMES_PS "s"
#endif

#endif
