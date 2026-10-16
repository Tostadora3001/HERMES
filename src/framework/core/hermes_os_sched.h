#ifndef HERMES_OS_SCHED_H
#define HERMES_OS_SCHED_H

/* The interface every scheduler of the OS library implements (one per
 * import: os_priority, os_deadline, os_cyclic). Exactly
 * one scheduler is linked into a program: it defines the constant
 * `hermes_os_scheduler`, which hermes_os.c calls, and also
 * hermes_os_set_priority (t.priority(p), hermes_os.h) -- a fatal error in
 * a scheduler without priorities. Not HERMESPY-facing.
 *
 * Rules for a scheduler:
 *   - every function is called with the framework lock held;
 *   - the threads it holds as "ready" must sit in hermes_wait_queue_t
 *     lists, added with hermes_queue_push: that way the thread library
 *     can take a thread out (abort, destroy, pause) with
 *     hermes_queue_unlink without knowing anything about the scheduler;
 *   - pick() must respect hermes_os_may_run_on() (pin / migrate) and skip
 *     paused threads, and take the thread it returns out of its lists. */

#include "hermes_thread.h"

typedef struct {
    const char *name;
    /* t is runnable (new, woken, or it just yielded): keep it */
    void (*ready)(hermes_thread_t *t);
    /* the next thread for `core`, taken out of the ready set; NULL = none */
    hermes_thread_t *(*pick)(int core);
    /* The OS tick (every os_sched_ticks() ticks), once per core running a
     * process thread: who should be on `core` now, given that `running`
     * is? Return `running` to keep it, another thread to want that one
     * instead, or NULL if `running` must leave the core even though
     * nobody replaces it. Anything but `running` makes the OS ask it to
     * give the core back (at its next safe point; see
     * hermes_sched_request_switch); pick() then chooses for real.
     * Look, don't touch: the ready set stays as it is. Threads in the
     * ready set aren't running anywhere (pick took the running ones out);
     * `taken` holds the `ntaken` threads already wanted for other cores in
     * this same tick: don't choose them again. */
    hermes_thread_t *(*decide)(int core, hermes_thread_t *running,
                               hermes_thread_t *const *taken, int ntaken);
    /* Optional (NULL = none). start: os_run() is about to start the OS
     * (lock held exactly once; may be a fatal error). created: below.
     * due: from the tick,
     * every OS tick -- has the scheduler work of its own due at OS ms
     * `now`? Cheap: it runs in the interrupt. work: the OS does it, in
     * its own context (see os_work in hermes_os.c), lock held exactly
     * once; it may release the lock meanwhile (e.g. to print), must
     * hold it again when it returns, and can't block or yield. */
    void (*start)(void);
    /* Optional: from the tick, every ms, for each core running a process
     * thread (`running`; lock held, interrupt context on bare metal):
     * that ms counts as used by it. Cheap; anything to report is left for
     * work() (make due() say so). */
    void (*charge)(hermes_thread_t *running);
    /* Optional: t left its core without staying ready -- it blocked, went
     * to sleep, was paused or finished (when it's ready again, ready(t)). */
    void (*left)(hermes_thread_t *t);
    /* t was just created (before ready(t)); the creator is the current
     * thread */
    void (*created)(hermes_thread_t *t);
    int (*due)(unsigned long now);
    void (*work)(unsigned long now);
} hermes_os_scheduler_t;

/* On AVR the table lives in flash (a const in RAM would cost its 20 B of
 * the little heap there: measured, the third light thread of
 * os_inherit didn't fit); read it with HERMES_SCHED(field). */
#if defined(__AVR__)
#include <avr/pgmspace.h>
#define HERMES_SCHED_TABLE PROGMEM
#define HERMES_SCHED(f) ((__typeof__(hermes_os_scheduler.f))pgm_read_word(&hermes_os_scheduler.f))
#else
#define HERMES_SCHED_TABLE
#define HERMES_SCHED(f) (hermes_os_scheduler.f)
#endif

extern const hermes_os_scheduler_t hermes_os_scheduler HERMES_SCHED_TABLE;

/* May `t` run on `core` now? next_core (one-shot migrate) wins over pin. */
static inline int hermes_os_may_run_on(const hermes_thread_t *t, int core) {
    int want = t->next_core >= 0 ? t->next_core : t->pin;
    return want < 0 || want == core;
}

static inline int hermes_os_is_taken(const hermes_thread_t *t, hermes_thread_t *const *taken, int ntaken) {
    for (int i = 0; i < ntaken; i++) if (taken[i] == t) return 1;
    return 0;
}

/* From hermes_os.c, for the schedulers. The OS's ms counter (lock held). */
unsigned long hermes_os_now_ms(void);
/* Lock held exactly once, by a process thread while the OS runs: it sleeps
 * (BLOCKED, as in delay_ms) until the OS's ms counter reaches `at`;
 * returns with the lock RELEASED once woken. */
void hermes_os_sleep_until_locked(unsigned long at);

#endif
