#ifndef HERMES_TIME_H
#define HERMES_TIME_H

/* `import hermes_time`: the time base for .hepy programs (hepyc links
 * hermes_drv_timer.c automatically). Plain functions, not a device: there
 * is no receiver -- "now" and "wait" don't belong to any instance.
 *
 *   delay_ms(ms)  wait, letting other hermes_threads run meanwhile
 *   delay_us(us)  short precise busy wait (never yields)
 *   millis()      ms since the timer started (first use)
 *   micros()      us since the timer started
 *
 * `long` because that's HERMESPY's integer type: on AVR (32-bit long) the
 * value turns negative after ~24.8 days (millis) / ~35.8 min (micros);
 * subtracting two readings still gives the right interval. See
 * hermes_drv_timer.h and docs/raw/hermes_drivers.en.md. */

#include "hermes_drv_timer.h"

static inline void delay_ms(long ms) { if (ms > 0) hermes_timer_delay_ms((uint32_t)ms); }
static inline void delay_us(long us) { if (us > 0) hermes_timer_delay_us((uint32_t)us); }
static inline long millis(void) { return (long)hermes_timer_millis(); }
static inline long micros(void) { return (long)hermes_timer_micros(); }

#endif
