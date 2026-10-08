#if !defined(__AVR__)
#define _POSIX_C_SOURCE 200809L /* clock_gettime, nanosleep */
#endif
#include "hermes_drv_timer.h"

/* Weak: resolves to NULL when the thread runtime (hermes_thread.c) isn't
 * linked -- see the header. */
extern void hermes_thread_yield(void) __attribute__((weak));

#if defined(__AVR__)

#include <avr/interrupt.h>
#include <avr/io.h>

#ifndef F_CPU
#error "hermes_drv_timer: F_CPU no definido (hepyc lo fija en el perfil avr)"
#endif

#define HERMES_TIMER_PRESCALER 64UL
#define HERMES_TIMER_TICKS_PER_MS (F_CPU / HERMES_TIMER_PRESCALER / 1000UL)
#if HERMES_TIMER_TICKS_PER_MS < 2 || HERMES_TIMER_TICKS_PER_MS > 256
#error "hermes_drv_timer: F_CPU fuera de rango para Timer0 con prescaler 64 a 1 ms"
#endif

static volatile uint32_t g_ms = 0;
static uint8_t g_started = 0;

ISR(TIMER0_COMPA_vect) {
    g_ms++;
}

void hermes_timer_init(void) {
    if (g_started) return;
    g_started = 1;
    TCCR0A = (1 << WGM01);              /* CTC: TOP = OCR0A */
    OCR0A = (uint8_t)(HERMES_TIMER_TICKS_PER_MS - 1);
    TCNT0 = 0;
    TIMSK0 = (1 << OCIE0A);
    TCCR0B = (1 << CS01) | (1 << CS00); /* clk/64: starts counting */
    sei();
}

uint32_t hermes_timer_millis(void) {
    hermes_timer_init();
    uint8_t sreg = SREG;
    cli();
    uint32_t ms = g_ms; /* 4-byte read: must not be torn by the ISR */
    SREG = sreg;
    return ms;
}

uint32_t hermes_timer_micros(void) {
    hermes_timer_init();
    uint8_t sreg = SREG;
    cli();
    uint32_t ms = g_ms;
    uint8_t ticks = TCNT0;
    /* A compare match already happened (counter wrapped to 0) but its ISR
     * hasn't run yet because interrupts are off: count that millisecond.
     * Only trusted when `ticks` is small -- if it's near TOP, it was read
     * before the wrap and the pending flag belongs to after the read. */
    if ((TIFR0 & (1 << OCF0A)) && ticks < HERMES_TIMER_TICKS_PER_MS / 2) ms++;
    SREG = sreg;
    return ms * 1000UL + ((uint32_t)ticks * HERMES_TIMER_PRESCALER) / (F_CPU / 1000000UL);
}

#else /* host */

#include <time.h>

static struct timespec g_base;
static int g_started = 0;

void hermes_timer_init(void) {
    if (g_started) return;
    g_started = 1;
    clock_gettime(CLOCK_MONOTONIC, &g_base);
}

static uint64_t elapsed_us(void) {
    hermes_timer_init();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)(now.tv_sec - g_base.tv_sec) * 1000000ULL
         + (uint64_t)((now.tv_nsec - g_base.tv_nsec) / 1000);
}

uint32_t hermes_timer_millis(void) {
    return (uint32_t)(elapsed_us() / 1000ULL);
}

uint32_t hermes_timer_micros(void) {
    return (uint32_t)elapsed_us();
}

#endif

void hermes_timer_delay_ms(uint32_t ms) {
    uint32_t start = hermes_timer_millis();
    while (hermes_timer_millis() - start < ms) {
        if (hermes_thread_yield) hermes_thread_yield();
#if !defined(__AVR__)
        /* host: don't spin a real core at 100% while waiting */
        struct timespec nap = { 0, 100000 }; /* 100 us */
        nanosleep(&nap, NULL);
#endif
    }
}

void hermes_timer_delay_us(uint32_t us) {
    uint32_t start = hermes_timer_micros();
    while (hermes_timer_micros() - start < us) {
    }
}
