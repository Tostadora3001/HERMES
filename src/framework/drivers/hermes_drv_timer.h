#ifndef HERMES_DRV_TIMER_H
#define HERMES_DRV_TIMER_H

/* Core driver: time base. Monotonic milli/microsecond counters since the
 * timer started, and delays.
 *
 * AVR: Timer0 in CTC mode, prescaler 64, one compare-match interrupt per
 * millisecond (OCR0A = F_CPU/64/1000 - 1 = 249 at 16 MHz). Timer0 is
 * therefore OWNED by this driver whenever it's linked: nothing else may
 * reconfigure it (no PWM on D5/D6, which are Timer0's outputs). micros()
 * adds TCNT0, so its resolution is one timer tick (4 us at 16 MHz).
 * F_CPU must be defined (hepyc's avr profile sets it) and satisfy
 * F_CPU/64000 <= 256 -- true for 8/12/16 MHz, not for 20 MHz.
 *
 * Host: CLOCK_MONOTONIC.
 *
 * The timer starts on first use (any function below) -- a program that
 * links this driver but never calls it never enables the interrupt.
 * Counters are uint32_t: millis wraps after ~49.7 days, micros after
 * ~71.6 minutes; differences `now - start` stay correct across one wrap.
 *
 * Interaction with hermes_threads: hermes_timer_delay_ms yields to other
 * cooperative threads while it waits (it doesn't hog the CPU). The yield
 * is a weak reference, so this driver works -- as a plain busy wait --
 * in programs that don't link the thread runtime at all.
 * hermes_timer_delay_us never yields: it's meant for short, precise
 * waits (e.g. a 10 us trigger pulse). */

#include <stdint.h>

void hermes_timer_init(void);
uint32_t hermes_timer_millis(void);
uint32_t hermes_timer_micros(void);
void hermes_timer_delay_ms(uint32_t ms);
void hermes_timer_delay_us(uint32_t us);

#endif
