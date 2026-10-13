#ifndef HERMES_SEM_H
#define HERMES_SEM_H

#include <stdbool.h>

/* Counting semaphore for guarding shared resources between threads,
 * including threads running at the same time on different cores (every
 * operation takes the framework lock, see hermes_sched_internal.h).
 * A wait() with no token gives control back to whoever ran the thread
 * (main waits right there) until a signal() hands it one. */
typedef struct hermes_sem hermes_sem_t;

/* Returns NULL if there is no free memory left. Unlike
 * hermes_thread_create, this does NOT abort for you -- check the return
 * value. */
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
