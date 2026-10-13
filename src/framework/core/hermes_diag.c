#include "hermes_diag.h"

/* Bare metal + UART only (see hermes_diag.h); elsewhere this file is empty. */
#if HERMES_BARE_METAL && defined(HERMES_DIAG_UART)

#include <stdarg.h>
#include <stdint.h>

void hermes_putchar(char c); /* the UART driver's */

/* one byte of a HERMES_PSTR / format string: in flash on AVR */
#if defined(__AVR__)
#define FLASH_BYTE(p) ((char)pgm_read_byte(p))
#else
#define FLASH_BYTE(p) (*(p))
#endif

static void put_str(const char *s) { /* s in RAM */
    if (!s) { hermes_putchar('?'); return; }
    while (*s) hermes_putchar(*s++);
}

static void put_str_P(const char *s) { /* s from HERMES_PSTR */
    if (!s) { hermes_putchar('?'); return; }
    char c;
    while ((c = FLASH_BYTE(s++))) hermes_putchar(c);
}

static void put_unsigned(unsigned long v, unsigned char base) {
    char buf[11];
    unsigned char i = 0;
    do {
        unsigned char d = (unsigned char)(v % base);
        buf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    while (i) hermes_putchar(buf[--i]);
}

void hermes_diag_P(const char *fmt_P, ...) {
    va_list ap;
    va_start(ap, fmt_P);
    char c;
    while ((c = FLASH_BYTE(fmt_P++))) {
        if (c != '%') { hermes_putchar(c); continue; }
        char len = 0;
        c = FLASH_BYTE(fmt_P++);
        if (c == 'l' || c == 'z') { len = c; c = FLASH_BYTE(fmt_P++); }
        switch (c) {
            case 'd': {
                long v = len == 'l' ? va_arg(ap, long) : (long)va_arg(ap, int);
                if (v < 0) { hermes_putchar('-'); v = -v; }
                put_unsigned((unsigned long)v, 10);
                break;
            }
            case 'u': /* %zu: size_t is an unsigned int on AVR and RV32 */
                put_unsigned(len == 'l' ? va_arg(ap, unsigned long)
                                        : (unsigned long)va_arg(ap, unsigned int), 10);
                break;
            case 'p':
                hermes_putchar('0');
                hermes_putchar('x');
                put_unsigned((unsigned long)(uintptr_t)va_arg(ap, void *), 16);
                break;
            case 's': put_str(va_arg(ap, const char *)); break;
            case 'S': put_str_P(va_arg(ap, const char *)); break;
            case 'c': hermes_putchar((char)va_arg(ap, int)); break;
            case '%': hermes_putchar('%'); break;
            default:  hermes_putchar('%'); if (c) hermes_putchar(c); else fmt_P--; break;
        }
    }
    va_end(ap);
}

#else
typedef int hermes_diag_unused_t; /* ISO C: no empty translation unit */
#endif
