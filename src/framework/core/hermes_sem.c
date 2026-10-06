#include "hermes_sem.h"
#include "hermes_sched_internal.h"

struct hermes_sem {
    long count;
    hermes_wait_queue_t waiters;
};

hermes_sem_t *hermes_sem_create(long initial_count) {
    hermes_sem_t *sem = hermes_runtime_alloc(sizeof(hermes_sem_t));
    if (!sem) {
        return NULL; /* caller's choice how to handle it, unlike hermes_thread_create's abort() */
    }
    sem->count = initial_count;
    sem->waiters.head = NULL;
    sem->waiters.tail = NULL;
    return sem;
}

void hermes_sem_destroy(hermes_sem_t *sem) {
    hermes_runtime_free(sem);
}

void hermes_sem_wait(hermes_sem_t *sem) {
    sem->count--;
    if (sem->count < 0) {
        hermes_sched_block_current(&sem->waiters); /* returns once woken by signal() */
    }
}

bool hermes_sem_try_wait(hermes_sem_t *sem) {
    if (sem->count > 0) {
        sem->count--;
        return true;
    }
    return false;
}

void hermes_sem_signal(hermes_sem_t *sem) {
    sem->count++;
    if (sem->count <= 0) {
        hermes_sched_wake_one(&sem->waiters);
    }
}
