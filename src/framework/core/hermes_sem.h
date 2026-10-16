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

/* Priority inheritance (under hermes_os; off by default, declared per
 * semaphore -- a semaphore has no owner, so the program says who a thread
 * blocked in wait() is waiting for). Both can be used on one semaphore;
 * call them right after creating it.
 *
 * inherit_holders: the threads holding a token -- that took one (wait,
 * try_wait, or handed one by signal) and haven't given it back with their
 * own signal -- run at least as urgently as the most urgent waiter. For a
 * semaphore guarding a resource (a pool of N). Followed in a table sized
 * now: one slot per token the semaphore has (at least 1, at most 255); a
 * holder beyond that isn't followed. A thread that only ever waits (a
 * consumer) keeps counting as a holder of what it took: don't use it on a
 * producer -> consumer semaphore.
 *
 * signaled_by(t): t, the thread that signals it, runs at least as urgently
 * as the most urgent waiter. For a producer -> consumer semaphore. One
 * thread per semaphore (a new call replaces it); forgotten when t ends.
 *
 * With either, a signal() that hands the token to a more urgent thread
 * gives that one the core right away (as mutex_unlock does). Out of
 * memory is a fatal error. */
struct hermes_thread;
void hermes_sem_inherit_holders(hermes_sem_t *sem);
void hermes_sem_signaled_by(hermes_sem_t *sem, struct hermes_thread *t);

#endif
