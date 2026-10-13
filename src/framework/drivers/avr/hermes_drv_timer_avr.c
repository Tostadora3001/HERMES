/* Timer driver, AVR implementation: Timer0, one interrupt per ms (same
 * registers on the ATmega328P and the ATmega2560). See hermes_drv_timer.h. */
#include "hermes_drv_timer.h"
#include <avr/interrupt.h>
#include <avr/io.h>

#ifndef F_CPU
#error "hermes_drv_timer: F_CPU not defined (hepyc sets it from the board's f_cpu)"
#endif

#define HERMES_TIMER_PRESCALER 64UL
#define HERMES_TIMER_TICKS_PER_MS (F_CPU / HERMES_TIMER_PRESCALER / 1000UL)
#if HERMES_TIMER_TICKS_PER_MS < 2 || HERMES_TIMER_TICKS_PER_MS > 256
#error "hermes_drv_timer: F_CPU out of range for Timer0 with prescaler 64 at 1 ms"
#endif

static volatile uint32_t g_ms = 0;
static uint8_t g_started = 0;
static void (*volatile g_tick)(void) = 0; /* hermes_timer_set_tick */

/* The tick may switch threads (hermes_os preemption): the interrupted
 * thread then stays inside tick() until it's resumed, with what the ISR
 * prologue saved (SREG, call-clobbered registers) and its return address
 * on its own stack; the call-saved ones are kept by the context switch.
 * Meanwhile other threads take this interrupt on their own stacks. */
ISR(TIMER0_COMPA_vect) {
    g_ms++;
    void (*tick)(void) = g_tick;
    if (tick) tick();
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

void hermes_timer_idle(void) {
}

void hermes_timer_set_tick(void (*fn)(void)) {
    hermes_timer_init();
    uint8_t sreg = SREG;
    cli();
    g_tick = fn; /* 2-byte write: not torn by the ISR */
    SREG = sreg;
}
