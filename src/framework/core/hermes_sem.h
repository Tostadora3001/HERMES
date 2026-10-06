#ifndef HERMES_SEM_H
#define HERMES_SEM_H

#include <stdbool.h>

/* Counting semaphore for guarding shared resources between threads.
 * Cooperative: there is only ever one thread actually running at a time
 * (see hermes_thread.h), so wait()/signal() need no atomics or interrupt
 * masking -- correctness comes from never yielding/switching in the middle
 * of an update to `count` or the wait queue. */
typedef struct hermes_sem hermes_sem_t;

/* Returns NULL if the runtime allocator is exhausted (always possible when
 * a memory-profile import -- 'light'/'medium'/'heavy' -- is active; see
 * docs/raw/hermes_mempool.en.md). Unlike hermes_thread_create, this does
 * NOT abort for you -- check the return value. */
hermes_sem_t *hermes_sem_create(long initial_count);
void hermes_sem_destroy(hermes_sem_t *sem);

/* P operation: if a permit is available, takes it and returns immediately;
 * otherwise blocks the calling thread (yielding to the scheduler) until
 * another thread calls hermes_sem_signal. */
void hermes_sem_wait(hermes_sem_t *sem);

/* Non-blocking P: takes a permit and returns true if one was immediately
 * available, otherwise returns false without blocking. */
bool hermes_sem_try_wait(hermes_sem_t *sem);

/* V operation: releases a permit, waking one blocked thread if any was
 * waiting. Never blocks. */
void hermes_sem_signal(hermes_sem_t *sem);

#endif
