#ifndef HERMES_MUTEX_H
#define HERMES_MUTEX_H

#include <stdbool.h>

/* Mutex: a lock with an owner -- the thread that took it, and only that
 * one, gives it back. Unlike a semaphore (a count nobody owns), knowing
 * the owner lets the OS do priority inheritance (under hermes_os): while
 * a more urgent thread waits for the mutex, the owner runs with that
 * thread's priority, so a less urgent one in between can't hold them both
 * up (priority inversion). Passed on along chains: if the owner itself
 * waits for another mutex (or a thread in wait(), or a semaphore with
 * inheritance), that one is raised too (hermes_inherit.c). Without
 * hermes_os priorities aren't used, and it's just a lock.
 *
 * Rules (each broken one is a fatal error, with its message):
 *   - not recursive: lock() by its own owner is an error (it would wait
 *     for itself forever); so is a lock() that closes a cycle of threads
 *     waiting for each other's mutexes (a deadlock);
 *   - unlock() only by its owner;
 *   - destroy() only while nobody holds it.
 * A thread that ends (returns, or is aborted) while holding mutexes gives
 * them back: each goes to its next waiter.
 *
 * unlock() hands the mutex straight to the most urgent waiter (the oldest
 * among equals). Under hermes_os, if that one is more urgent than the
 * caller is left, the caller gives its core back right away. */
typedef struct hermes_mutex hermes_mutex_t;

/* NULL if there is no free memory (like hermes_sem_create: check it). */
hermes_mutex_t *hermes_mutex_create(void);
void hermes_mutex_destroy(hermes_mutex_t *m);
void hermes_mutex_lock(hermes_mutex_t *m);
bool hermes_mutex_try_lock(hermes_mutex_t *m);
void hermes_mutex_unlock(hermes_mutex_t *m);

#endif
