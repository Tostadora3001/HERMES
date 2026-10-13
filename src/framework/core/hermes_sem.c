#include "hermes_sem.h"
#include "hermes_sched_internal.h"

/* `count` = tokens available, never negative. A signal with someone
 * waiting hands the token straight to that thread instead of counting it.
 * (Before, count went negative by one per waiter; a thread aborted while
 * waiting -- hermes_thread_abort -- then left its -1 in the count for good.
 * With this form an aborted waiter is simply unlinked and leaves no trace.)
 * Every operation runs under the framework lock: threads on different
 * cores can use the same semaphore. */
struct hermes_sem {
    long count;
    hermes_wait_queue_t waiters;
};

hermes_sem_t *hermes_sem_create(long initial_count) {
    hermes_sem_t *sem = hermes_runtime_alloc(sizeof(hermes_sem_t));
    if (!sem) {
        return NULL; /* caller's choice how to handle it, unlike hermes_thread_create's fatal error */
    }
    sem->count = initial_count;
    sem->waiters.head = NULL;
    sem->waiters.tail = NULL;
    return sem;
}

void hermes_sem_destroy(hermes_sem_t *sem) {
    hermes_runtime_free(sem);
}

/* Host: before blocking, retry a few times -- another core usually gives
 * the token back within a few hundred cycles, much cheaper than blocking
 * (switching away and being woken). Pointless with one core. */
#if !HERMES_BARE_METAL
#define HERMES_SEM_SPINS 200
#else
#define HERMES_SEM_SPINS 0
#endif

void hermes_sem_wait(hermes_sem_t *sem) {
    hermes_lock();
    for (int spin = 0; spin < HERMES_SEM_SPINS && sem->count == 0 && hermes_cores_count() > 1; spin++) {
        hermes_unlock();
        hermes_lock();
    }
    if (sem->count > 0) {
        sem->count--;
        hermes_unlock();
        return;
    }
    hermes_sched_block_current(&sem->waiters); /* returns, unlocked, once signal() hands it a token */
}

bool hermes_sem_try_wait(hermes_sem_t *sem) {
    hermes_lock();
    bool got = sem->count > 0;
    if (got) sem->count--;
    hermes_unlock();
    return got;
}

void hermes_sem_signal(hermes_sem_t *sem) {
    hermes_lock();
    if (!hermes_sched_wake_one(&sem->waiters)) {
        sem->count++; /* nobody (alive) waiting: keep the token */
    }
    hermes_unlock();
}
