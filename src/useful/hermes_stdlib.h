#ifndef HERMES_STDLIB_H
#define HERMES_STDLIB_H

#include <stdbool.h>

#if defined(__AVR__)

/* No printf here on purpose: avr-libc's vfprintf/printf formatting engine
 * costs ~1.5 KB of flash on its own (measured with avr-size on an
 * atmega328p -- see docs/raw/hermes_stdlib.en.md), paid by every program
 * that prints anything at all, regardless of how little formatting it
 * actually needs. These are hand-rolled, minimal equivalents instead.
 *
 * `hermes_putchar` is the single point where a byte actually leaves the
 * MCU. It's `weak` so it links even with nothing wired up yet (the default
 * does nothing) -- once src/framework/drivers has a real UART driver, a
 * program (or that driver) defines a real `hermes_putchar` and output
 * starts appearing with no other change needed here. */
__attribute__((weak)) void hermes_putchar(char c) { (void)c; }

static inline void hermes_print_str(const char *v) {
    while (*v) hermes_putchar(*v++);
    hermes_putchar('\n');
}

static inline void hermes_print_bool(bool v) {
    hermes_print_str(v ? "True" : "False");
}

static inline void hermes_print_long(long v) {
    char buf[12]; /* max 10 digits for a 32-bit long + sign, no NUL needed */
    int i = 0;
    unsigned long uv = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    if (uv == 0) buf[i++] = '0';
    while (uv > 0) { buf[i++] = (char)('0' + (uv % 10)); uv /= 10; }
    if (v < 0) hermes_putchar('-');
    while (i > 0) hermes_putchar(buf[--i]);
    hermes_putchar('\n');
}

/* Fixed-point only (integer part + 4 decimals), no exponent notation --
 * good enough for simple diagnostics without pulling in float-formatting
 * printf support. */
static inline void hermes_print_double(double v) {
    if (v < 0) { hermes_putchar('-'); v = -v; }
    unsigned long ip = (unsigned long)v;
    double frac = v - (double)ip;

    char buf[12]; int i = 0;
    if (ip == 0) buf[i++] = '0';
    while (ip > 0) { buf[i++] = (char)('0' + (ip % 10)); ip /= 10; }
    while (i > 0) hermes_putchar(buf[--i]);

    hermes_putchar('.');
    for (int d = 0; d < 4; d++) {
        frac *= 10;
        int digit = (int)frac;
        hermes_putchar((char)('0' + digit));
        frac -= digit;
    }
    hermes_putchar('\n');
}

#else /* host: plain printf, no memory pressure that justifies avoiding it */

#include <stdio.h>

static inline void hermes_print_long(long v) { printf("%ld\n", v); }
static inline void hermes_print_double(double v) { printf("%g\n", v); }
static inline void hermes_print_str(const char *v) { printf("%s\n", v); }
static inline void hermes_print_bool(bool v) { printf("%s\n", v ? "True" : "False"); }

#endif

/* `print(x)` en HERMESPY se traduce literalmente a una llamada `print(x)` en
 * C; esta macro resuelve, segun el tipo de `x`, a la funcion propia
 * correspondiente (todas definidas en este mismo archivo, en C). */
#define print(x) _Generic((x), \
    int: hermes_print_long, \
    long: hermes_print_long, \
    float: hermes_print_double, \
    double: hermes_print_double, \
    char*: hermes_print_str, \
    const char*: hermes_print_str, \
    bool: hermes_print_bool \
)(x)

#endif
