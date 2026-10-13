#ifndef HERMES_STDLIB_H
#define HERMES_STDLIB_H

#include <stdbool.h>
#include "hermes_platform.h"

#if HERMES_BARE_METAL

/* Bare metal (AVR, RISC-V microcontrollers: hermes_platform.h).
 * No printf here on purpose: avr-libc's vfprintf/printf formatting engine
 * costs ~1.5 KB of flash on its own (measured with avr-size on an
 * atmega328p -- see docs/raw/hermes_stdlib.en.md), paid by every program
 * that prints anything at all, regardless of how little formatting it
 * actually needs. These are hand-rolled, minimal equivalents instead
 * (also on RISC-V: same output on every microcontroller, no stdio).
 *
 * `hermes_putchar` is the single point where a byte actually leaves the
 * MCU. It's `weak` so it links even with nothing wired up yet (the default
 * does nothing) -- once src/framework/drivers has a real UART driver, a
 * program (or that driver) defines a real `hermes_putchar` and output
 * starts appearing with no other change needed here. */
__attribute__((weak)) void hermes_putchar(char c) { (void)c; }

/* A line comes out whole: under hermes_os the tick may take the core from
 * a thread anywhere (preemption), and another thread's line would land in
 * the middle. The thread runtime's hermes_preempt_off/on put the switch
 * off until the line is done (interrupts stay enabled). Weak references:
 * without the runtime they are NULL and print() doesn't need them. */
extern void hermes_preempt_off(void) __attribute__((weak));
extern void hermes_preempt_on(void) __attribute__((weak));
/* One copy each, not inlined in every print() (-70 B of flash on AVR in
 * os_priority_demo); `unused` because a program may not print at all. */
__attribute__((noinline, unused)) static void hermes_print_begin(void) {
    if (hermes_preempt_off) hermes_preempt_off();
}
__attribute__((noinline, unused)) static void hermes_print_end(void) {
    hermes_putchar('\n');
    if (hermes_preempt_on) hermes_preempt_on();
}

static inline void hermes_print_str(const char *v) {
    hermes_print_begin();
    while (*v) hermes_putchar(*v++);
    hermes_print_end();
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
    hermes_print_begin();
    if (v < 0) hermes_putchar('-');
    while (i > 0) hermes_putchar(buf[--i]);
    hermes_print_end();
}

/* Fixed-point only (integer part + 4 decimals), no exponent notation --
 * good enough for simple diagnostics without pulling in float-formatting
 * printf support. */
static inline void hermes_print_double(double v) {
    hermes_print_begin();
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
    hermes_print_end();
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
