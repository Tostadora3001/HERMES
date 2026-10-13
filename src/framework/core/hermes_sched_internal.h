#ifndef HERMES_SCHED_INTERNAL_H
#define HERMES_SCHED_INTERNAL_H

/* Runtime hooks shared between framework modules (hermes_thread.c,
 * hermes_sem.c, hermes_parallel.c, hermes_device.c and -- for
 * hermes_runtime_alloc/_free only -- device constructors such as
 * modules/gpio/gpio.h). Not part of the HERMESPY-facing API: no .hepy
 * program should ever call these directly. */

#include "hermes_thread.h"
#include "hermes_platform.h"

/* Most cores the runtime handles: bare metal has one; on the host, one per
 * hardware thread, up to this many. */
#if HERMES_BARE_METAL
#undef HERMES_MAX_CORES
#define HERMES_MAX_CORES 1
#elif !defined(HERMES_MAX_CORES)
#define HERMES_MAX_CORES 64
#endif

/* --- the framework lock ---------------------------------------------------
 * One lock for all shared runtime state (thread states, semaphores, launch
 * queues, device lists, the allocator). Re-entrant: the same core may
 * take it again while holding it.
 *   host: a recursive pthread mutex (several cores run at once);
 *   AVR:  interrupts off (one core; also keeps ISRs out). */
void hermes_lock(void);
void hermes_unlock(void);

/* Called with the lock held (exactly once). Marks the current thread
 * BLOCKED, enqueues it onto `q` and gives control back to its resumer; on
 * main (or another root context) it waits right there until woken.
 * Returns -- with the lock RELEASED -- once hermes_sched_wake_one handed it
 * a token. If no core can make progress any more it's a deadlock: fatal. */
void hermes_sched_block_current(hermes_wait_queue_t *q);

/* Called with the lock held. Pops one live thread from `q` (FIFO), if any,
 * and marks it READY: its resumer (or itself, on a root) goes on with it.
 * Returns 1 if a thread was woken, 0 if `q` was empty. */
int hermes_sched_wake_one(hermes_wait_queue_t *q);

/* For hermes_parallel.c: if `t` is still waiting in a launch queue, takes
 * it out and binds it to the calling core. Returns 1 if taken. */
int hermes_thread_take(hermes_thread_t *t);

/* For hermes_parallel.c: runs `t` on the calling core until it finishes,
 * resuming it every time it yields and waiting while it's blocked. */
void hermes_thread_run_to_end(hermes_thread_t *t);

/* --- hooks for the OS library (hermes_os.c, `import hermes_os`) ---------
 * NULL without it. hermes_os.c sets it from a constructor. All but the
 * block functions are called with the lock held. */
typedef struct {
    void *(*block_alloc)(size_t n);   /* where a thread's block lives */
    void (*block_free)(void *p);
    void (*created)(hermes_thread_t *t);     /* a new thread joined the process */
    void (*ready)(hermes_thread_t *t);       /* t became runnable: to the scheduler */
    void (*terminated)(hermes_thread_t *t);  /* t finished or was aborted */
    void (*core_loop)(void);  /* a core runs the scheduler until it stops */
    volatile unsigned char running;          /* os_run() in progress */
} hermes_os_hooks_t;

extern hermes_os_hooks_t *hermes_os_hooks;

/* For the OS library (lock held, exactly once): */
void hermes_sched_resume(hermes_thread_t *t); /* run t here until it gives control back */
void hermes_sched_idle(void);                 /* wait for a change (deadlock-checked) */
void hermes_sched_notify(void);               /* wake idle cores */
void hermes_cores_start(void);                /* start the other cores (host) */
void hermes_queue_push(hermes_wait_queue_t *q, hermes_thread_t *t);
void hermes_queue_unlink(hermes_thread_t *t);
void hermes_thread_check_live(hermes_thread_t *t, const char *op); /* fatal if destroyed */

/* Asks the thread running on `core` to give the core back (from the OS
 * tick, when the scheduler wants another thread there). Bare metal: the
 * tick's interrupt then calls hermes_sched_preempt, which switches right
 * there (preemption). Host: the thread does it at its next safe point --
 * the next time it takes the framework lock from outside it (any thread,
 * semaphore, device or OS operation, delay_ms...); a thread that never
 * enters the framework never gives the core back. Either way, not while
 * the thread is inside os_preempt_off() (it's done at the matching
 * os_preempt_on()). Cleared when the core resumes a thread
 * (hermes_sched_resume). Lock held or in the tick's interrupt. */
void hermes_sched_request_switch(int core);

/* From the tick's interrupt, lock not held: if a switch was requested for
 * this core, the interrupted thread gives it back right here -- it stays
 * inside the interrupt, on its own stack, until it's resumed, and then
 * the interrupt returns to where it was. No-op on the host. */
void hermes_sched_preempt(void);

/* No preemption (nor safe-point switches) for the current thread between
 * the two; nestable. A switch requested meanwhile happens at the last
 * hermes_preempt_on(). Interrupts stay enabled. (os_preempt_off/on in
 * HERMESPY; print() uses them on bare metal so a line comes out whole.) */
void hermes_preempt_off(void);
void hermes_preempt_on(void);

/* Set by hermes_fatal(): from then on nobody gives its core back (the
 * fatal path must finish on the thread that started it). */
extern volatile unsigned char hermes_fatal_started;

/* Single allocation point shared across the framework (implemented in
 * hermes_thread.c): malloc/free under the framework lock, plus the AVR
 * heap-limit fix. Memory comes from whatever is free; NULL when there is
 * none, same contract as malloc. */
void *hermes_runtime_alloc(size_t n);
void hermes_runtime_free(void *p);

#endif
