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

/* Called with the lock held. Same for one given BLOCKED thread, wherever
 * it waits (taken out of its queue). */
void hermes_sched_wake(hermes_thread_t *t);

/* Priority inheritance (hermes_inherit.c), lock held. hermes_inherit.c
 * is linked only with hermes_os (corelib.c): priorities only exist there,
 * and a program without the OS doesn't carry the code -- hence the weak
 * references (NULL without the OS).
 *
 * hermes_prio_recompute: something that decides who inherits what changed
 * (a thread blocked or woke, a mutex or a semaphore changed hands, a base
 * priority or deadline changed -- `t` is the thread concerned, a hint
 * only): every process thread's effective urgency is computed again --
 * its own, or the most urgent of the threads waiting for it, directly or
 * through a chain of waits -- and the OS told of each change.
 * hermes_inherit_wait_check: self (a process thread) is about to block in
 * wait() for t: a fatal error if t waits for self, directly or not
 * (through wait() and mutexes). */
struct hermes_mutex;
struct hermes_sem;
void hermes_inherit_recompute(void) __attribute__((weak));
void hermes_inherit_wait_check(hermes_thread_t *self, hermes_thread_t *t) __attribute__((weak));
static inline void hermes_prio_recompute(hermes_thread_t *t) {
    (void)t;
    if (hermes_inherit_recompute) hermes_inherit_recompute();
}
/* What a blocked thread waits for (hermes_thread_t.blocked_kind). */
#define HERMES_BLOCK_NONE   0
#define HERMES_BLOCK_MUTEX  1
#define HERMES_BLOCK_THREAD 2
#define HERMES_BLOCK_SEM    3
/* A mutex's owner (NULL = free); the k-th thread a semaphore with
 * inheritance passes urgency to (holders, then the declared signaler;
 * NULL = no more); the semaphore side of a thread that ends (it leaves
 * the holder tables and stops being a declared signaler). */
hermes_thread_t *hermes_mutex_owner(const struct hermes_mutex *m);
hermes_thread_t *hermes_sem_target(const struct hermes_sem *s, int k);
void hermes_sem_thread_gone(hermes_thread_t *t);
/* The single thread x waits for through a mutex (its owner) or wait()
 * (that thread); NULL for anything else (a semaphore: someone else may
 * still signal it). For deadlock checks: follow it from a thread. */
static inline hermes_thread_t *hermes_inherit_next(const hermes_thread_t *x) {
    if (x->blocked_kind == HERMES_BLOCK_MUTEX) return hermes_mutex_owner(x->blocked_on);
    if (x->blocked_kind == HERMES_BLOCK_THREAD) return (hermes_thread_t *)x->blocked_on;
    return NULL;
}

/* What "more urgent" means for mutexes (who gets a mutex first, what its
 * owner inherits): the effective priority (higher wins) -- or, with
 * os_deadline, the effective deadline (earlier wins: deadline
 * inheritance). EFF = effective, BASE = its own. */
#ifdef HERMES_OS_DEADLINE
typedef unsigned long hermes_urgency_t;
#define HERMES_URG_EFF(t)    ((t)->dl_eff)
#define HERMES_URG_NEW(t)    ((t)->dl_new)
#define HERMES_URG_BASE(t)   ((t)->dl_abs)
#define HERMES_URG_MORE(a, b) ((long)((a) - (b)) < 0)
#else
typedef unsigned char hermes_urgency_t;
#define HERMES_URG_EFF(t)    ((t)->priority)
#define HERMES_URG_NEW(t)    ((t)->prio_new)
#define HERMES_URG_BASE(t)   ((t)->base_priority)
#define HERMES_URG_MORE(a, b) ((a) > (b))
#endif

/* For hermes_inherit.c (lock held): t terminated holding mutexes: each
 * goes to its next waiter (or is left free). */
void hermes_mutex_thread_gone(hermes_thread_t *t);

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
    /* t's effective priority changed (mutex priority inheritance): if it
     * waits in the ready set, re-file it */
    void (*reprioritized)(hermes_thread_t *t);
    /* Some thread waits for time to pass (asleep in delay_ms): an idle core
     * isn't a deadlock, the tick will wake it -- bare metal: the CPU sleeps
     * until the next interrupt; host: no deadlock timeout. */
    int (*time_will_wake)(void);
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
/* A request for `core` not yet acted on. */
int hermes_sched_switch_pending(int core);

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
