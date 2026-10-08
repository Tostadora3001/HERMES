#ifndef HERMES_SCHED_INTERNAL_H
#define HERMES_SCHED_INTERNAL_H

/* Scheduler hooks shared between framework modules (hermes_thread.c,
 * hermes_sem.c, and -- for hermes_runtime_alloc/_free only --
 * device constructors such as modules/gpio/gpio.h). Not part of the
 * HERMESPY-facing API: no .hepy program should ever call these directly. */

#include "hermes_thread.h"

typedef struct {
    hermes_thread_t *head;
    hermes_thread_t *tail;
} hermes_wait_queue_t;

/* Marks the current thread BLOCKED, enqueues it onto `q`, and switches to
 * another ready thread. Aborts the process if no other thread is ready --
 * that situation is a genuine deadlock in this cooperative scheduler. */
void hermes_sched_block_current(hermes_wait_queue_t *q);

/* Pops one thread from `q` (FIFO), if any, marks it READY and enqueues it
 * onto the scheduler's ready queue. Does not switch context -- the caller
 * keeps running until it next yields/blocks/returns. Returns 1 if a thread
 * was woken, 0 if `q` was empty. */
int hermes_sched_wake_one(hermes_wait_queue_t *q);

/* Single allocation point shared across the framework, so every module
 * honors the same policy without needing to know which one is active.
 * Implemented in hermes_thread.c:
 *   - plain malloc/free if HERMES_POOL_BYTES is not defined;
 *   - a bump allocator over a static array of that many bytes if it is
 *     (see docs/raw/hermes_mempool.en.md) -- hermes_runtime_free is then a
 *     no-op, since a pure bump allocator never reclaims (matches today's
 *     behavior even in the malloc case: nothing frees a hermes_thread_t
 *     either, threads are assumed to live for the program's lifetime).
 * hermes_runtime_alloc returns NULL on exhaustion, same contract as malloc. */
void *hermes_runtime_alloc(size_t n);
void hermes_runtime_free(void *p);

#endif
