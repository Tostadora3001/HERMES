#ifndef HERMES_DRV_TIMER_H
#define HERMES_DRV_TIMER_H

/* Core driver: time base. Monotonic milli/microsecond counters since the
 * timer started, and delays.
 *
 * Common interface; one implementation per platform:
 *   hermes_drv_timer.c                   common part (delays)
 *   avr/hermes_drv_timer_avr.c           Timer0, interrupt every ms
 *   esp32c3/hermes_drv_timer_esp32c3.c   SYSTIMER (16 MHz counter; interrupt
 *                                        only for the tick)
 *   native/hermes_drv_timer_native.c     CLOCK_MONOTONIC (+ a pthread for the tick)
 * hepyc links the common part and the one for the target's platform (the
 * board's chip family: boards/<board>/board.hboard, key `platform`).
 *
 * AVR: Timer0 in CTC mode, prescaler 64, one compare-match interrupt per
 * millisecond (OCR0A = F_CPU/64/1000 - 1 = 249 at 16 MHz). Timer0 is
 * therefore OWNED by this driver whenever it's linked: nothing else may
 * reconfigure it (no PWM on D5/D6, which are Timer0's outputs). micros()
 * adds TCNT0, so its resolution is one timer tick (4 us at 16 MHz).
 * F_CPU must be defined (hepyc sets it from the board's f_cpu) and satisfy
 * F_CPU/64000 <= 256 -- true for 8/12/16 MHz, not for 20 MHz.
 *
 * Host: CLOCK_MONOTONIC.
 *
 * The system timer: Timer0 by default; `import hermes_timers` can give the
 * role to another timer, or to none (millis then stands still and
 * delay_ms never ends) -- see hermes_drv_timers.h and
 * docs/raw/hermes_timers.en.md. The host mock follows the role too:
 * without a system timer its millis/micros stand still.
 *
 * The timer starts on first use (any function below) -- a program that
 * links this driver but never calls it never enables the interrupt.
 * Counters are uint32_t: millis wraps after ~49.7 days, micros after
 * ~71.6 minutes; differences `now - start` stay correct across one wrap.
 *
 * Interaction with hermes_threads: hermes_timer_delay_ms calls
 * hermes_thread_yield on every check while it waits: inside a thread, that
 * gives control back to whoever runs it (who can do other work and run it
 * again); on main it does nothing (there's no scheduler). The yield is a
 * weak reference, so this driver works -- as a plain busy wait -- in
 * programs that don't link the thread runtime at all.
 * Under hermes_os, a process thread doesn't loop at all while os_run()
 * runs: it sleeps until the OS wakes it (hermes_os_sleep_ms, also a weak
 * reference).
 * hermes_timer_delay_us never yields: it's meant for short, precise
 * waits (e.g. a 10 us trigger pulse).
 *
 * The tick (hermes_timer_set_tick): a function called every millisecond
 * from the timer's interrupt -- the system tick the OS (hermes_os) runs
 * on. One at a time; NULL stops it. It runs in interrupt context: short,
 * never blocking, interrupts masked (bare metal: on the stack of whatever
 * was running). Per platform:
 *   AVR      Timer0's compare ISR (the same one that counts millis);
 *   ESP32-C3 SYSTIMER alarm 0 in periodic mode (1 ms), routed through the
 *            interrupt matrix to CPU interrupt HERMES_ESP32C3_TICK_INT; the
 *            first call installs the trap vector and enables interrupts --
 *            the only interrupt HERMES uses on this chip;
 *   host     a pthread that wakes every ms (absolute deadlines) and calls
 *            it: not an interrupt, so it may take a mutex. Stopping it
 *            waits for that pthread: never call set_tick(NULL) while holding
 *            something the tick function takes.
 * Call it from normal code (not from the tick itself), outside the
 * framework lock: on bare metal it enables interrupts. */

#include <stdint.h>

/* Implemented by each target. */
void hermes_timer_init(void);
uint32_t hermes_timer_millis(void);
uint32_t hermes_timer_micros(void);
/* What to do on each check while delay_ms waits, after yielding (AVR:
 * nothing; host: sleep 100 us, so as not to spin a real core at 100%).
 * Internal: for the common part. */
void hermes_timer_idle(void);

void hermes_timer_set_tick(void (*fn)(void));

/* 1 once the timer has started (any of the functions above was used). */
int hermes_timer_started(void);

/* Common part. */
void hermes_timer_delay_ms(uint32_t ms);
void hermes_timer_delay_us(uint32_t us);

#endif
