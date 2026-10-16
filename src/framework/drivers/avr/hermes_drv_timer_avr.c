/* Timer driver, AVR implementation: the system timer, one interrupt per
 * ms (same registers on the ATmega328P and the ATmega2560). See
 * hermes_drv_timer.h.
 *
 * The system timer is Timer0 unless `import hermes_timers` gives the
 * system role to another one (hermes_drv_timers.h): then hepyc passes
 * -DHERMES_TIMERS and this file can run it on any timer of the chip --
 * every timer in CTC mode, prescaler 64, compare A every ms. Without that
 * import only Timer0's code and interrupt exist (no cost). */
#include "hermes_drv_timer.h"
#include <avr/interrupt.h>
#include <avr/io.h>
#ifdef HERMES_TIMERS
#include "hermes_drv_timers.h"
#endif

#ifndef F_CPU
#error "hermes_drv_timer: F_CPU not defined (hepyc sets it from the board's f_cpu)"
#endif

#define HERMES_TIMER_PRESCALER 64UL
#define HERMES_TIMER_TICKS_PER_MS (F_CPU / HERMES_TIMER_PRESCALER / 1000UL)
#if HERMES_TIMER_TICKS_PER_MS < 2 || HERMES_TIMER_TICKS_PER_MS > 256
#error "hermes_drv_timer: F_CPU out of range for an 8-bit timer with prescaler 64 at 1 ms"
#endif

static volatile uint32_t g_ms = 0;
static uint8_t g_started = 0;
static void (*volatile g_tick)(void) = 0; /* hermes_timer_set_tick */
/* the timer counting millis now; 0xFF = none (hermes_timers took the role
 * away: millis stands still) */
static volatile uint8_t g_hw = 0;

/* The tick may switch threads (hermes_os preemption): the interrupted
 * thread then stays inside tick() until it's resumed, with what the ISR
 * prologue saved (SREG, call-clobbered registers) and its return address
 * on its own stack; the call-saved ones are kept by the context switch.
 * Meanwhile other threads take this interrupt on their own stacks. */
static inline void ms_interrupt(void) {
    g_ms++;
    void (*tick)(void) = g_tick;
    if (tick) tick();
}

ISR(TIMER0_COMPA_vect) { ms_interrupt(); }

/* --- the timers that can be the system timer -------------------------------
 * Timer0 always; the others only with hermes_timers. Each: start in CTC at
 * 1 ms with its compare-A interrupt, stop, read its count and whether a
 * compare is pending (for micros). Prescaler 64 is CS = 3 on Timer0/1/3-5
 * and CS = 4 on Timer2. */

static void start_hw(uint8_t n) {
    uint8_t top = (uint8_t)(HERMES_TIMER_TICKS_PER_MS - 1);
    switch (n) {
    case 0:
        TCCR0A = (1 << WGM01); /* CTC: TOP = OCR0A */
        OCR0A = top;
        TCNT0 = 0;
        TIFR0 = (1 << OCF0A);
        TIMSK0 = (1 << OCIE0A);
        TCCR0B = (1 << CS01) | (1 << CS00); /* clk/64: starts counting */
        break;
#ifdef HERMES_TIMERS
    case 2:
        TCCR2A = (1 << WGM21);
        OCR2A = top;
        TCNT2 = 0;
        TIFR2 = (1 << OCF2A);
        TIMSK2 = (1 << OCIE2A);
        TCCR2B = (1 << CS22); /* clk/64 on Timer2 */
        break;
#define HERMES_START16_(k)                                                     \
    case k:                                                                    \
        TCCR##k##A = 0;                                                        \
        OCR##k##A = top;                                                       \
        TCNT##k = 0;                                                           \
        TIFR##k = (1 << OCF##k##A);                                            \
        TIMSK##k = (1 << OCIE##k##A);                                          \
        TCCR##k##B = (1 << WGM##k##2) | (1 << CS##k##1) | (1 << CS##k##0);     \
        break;
#ifdef TCCR1A
    HERMES_START16_(1)
#endif
#ifdef TCCR3A
    HERMES_START16_(3)
#endif
#ifdef TCCR4A
    HERMES_START16_(4)
#endif
#ifdef TCCR5A
    HERMES_START16_(5)
#endif
#endif
    default: break;
    }
}

#ifdef HERMES_TIMERS /* only moving the system role stops a timer */
static void stop_hw(uint8_t n) {
    switch (n) {
    case 0: TIMSK0 = 0; TCCR0B = 0; TCCR0A = 0; break;
    case 2: TIMSK2 = 0; TCCR2B = 0; TCCR2A = 0; break;
#define HERMES_STOP16_(k) case k: TIMSK##k = 0; TCCR##k##B = 0; TCCR##k##A = 0; break;
#ifdef TCCR1A
    HERMES_STOP16_(1)
#endif
#ifdef TCCR3A
    HERMES_STOP16_(3)
#endif
#ifdef TCCR4A
    HERMES_STOP16_(4)
#endif
#ifdef TCCR5A
    HERMES_STOP16_(5)
#endif
    default: break;
    }
}
#endif

/* Interrupts off. Count since the last ms and whether a compare is
 * pending (its interrupt hasn't run yet). */
static uint8_t read_hw(uint8_t n, uint8_t *pending) {
    switch (n) {
    case 0: *pending = (TIFR0 & (1 << OCF0A)) != 0; return TCNT0;
#ifdef HERMES_TIMERS
    case 2: *pending = (TIFR2 & (1 << OCF2A)) != 0; return TCNT2;
#define HERMES_READ16_(k) case k: *pending = (TIFR##k & (1 << OCF##k##A)) != 0; return (uint8_t)TCNT##k;
#ifdef TCCR1A
    HERMES_READ16_(1)
#endif
#ifdef TCCR3A
    HERMES_READ16_(3)
#endif
#ifdef TCCR4A
    HERMES_READ16_(4)
#endif
#ifdef TCCR5A
    HERMES_READ16_(5)
#endif
#endif
    default: *pending = 0; return 0;
    }
}

#ifdef HERMES_TIMERS
#ifdef TIMER2_COMPA_vect
ISR(TIMER2_COMPA_vect) { ms_interrupt(); }
#endif
#ifdef TIMER1_COMPA_vect
ISR(TIMER1_COMPA_vect) { ms_interrupt(); }
#endif
#ifdef TIMER3_COMPA_vect
ISR(TIMER3_COMPA_vect) { ms_interrupt(); }
#endif
#ifdef TIMER4_COMPA_vect
ISR(TIMER4_COMPA_vect) { ms_interrupt(); }
#endif
#ifdef TIMER5_COMPA_vect
ISR(TIMER5_COMPA_vect) { ms_interrupt(); }
#endif

/* hermes_drv_timers: the system role moved to timer n (-1: to none). If
 * the timer already runs, millis goes on from where it was on the new one
 * (at most a fraction of a ms lost or gained at the switch). */
void hermes_timer_hw_system(int n) {
    uint8_t sreg = SREG;
    cli();
    uint8_t next = n < 0 ? 0xFF : (uint8_t)n;
    if (g_started && g_hw != 0xFF) stop_hw(g_hw);
    g_hw = next;
    if (g_started && next != 0xFF) start_hw(next);
    SREG = sreg;
}
#endif

void hermes_timer_init(void) {
    if (g_started) return;
    g_started = 1;
#ifdef HERMES_TIMERS
    int n = hermes_timers_system();
    g_hw = n < 0 ? 0xFF : (uint8_t)n;
#endif
    if (g_hw != 0xFF) start_hw(g_hw);
    sei();
}

int hermes_timer_started(void) { return g_started; }

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
    uint8_t pending = 0, ticks = 0;
    if (g_hw != 0xFF) ticks = read_hw(g_hw, &pending);
    /* A compare match already happened (counter wrapped to 0) but its ISR
     * hasn't run yet because interrupts are off: count that millisecond.
     * Only trusted when `ticks` is small -- if it's near TOP, it was read
     * before the wrap and the pending flag belongs to after the read. */
    if (pending && ticks < HERMES_TIMER_TICKS_PER_MS / 2) ms++;
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
