#include "hermes_mutex.h"
#include "hermes_sched_internal.h"

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on bare metal */

/* See hermes_mutex.h. Every operation runs under the framework lock.
 * Waiters sit in a hermes_wait_queue_t (so abort/destroy can take them
 * out without knowing about mutexes); each thread knows the mutex it waits
 * for (blocked_on, HERMES_BLOCK_MUTEX) and the ones it holds (held, linked
 * through next_held): that's all priority inheritance (hermes_inherit.c)
 * needs. */
struct hermes_mutex {
    hermes_thread_t *owner;         /* NULL = free */
    struct hermes_mutex *next_held; /* the owner's list */
    hermes_wait_queue_t waiters;
};

/* Every caller holds the lock exactly once. */
static void fatal(void) {
    hermes_unlock();
    hermes_fatal();
}

/* Priority inheritance itself is hermes_inherit.c's (one engine for
 * mutexes, wait() and semaphores); here, only who owns what. */

hermes_thread_t *hermes_mutex_owner(const hermes_mutex_t *m) { return m->owner; }

/* --- ownership (lock held) ------------------------------------------------ */

static void take(hermes_mutex_t *m, hermes_thread_t *t) {
    m->owner = t;
    m->next_held = t->held;
    t->held = m;
}

static void untake(hermes_mutex_t *m) {
    hermes_mutex_t **link = &m->owner->held;
    while (*link && *link != m) link = &(*link)->next_held;
    if (*link) *link = m->next_held;
    m->next_held = NULL;
    m->owner = NULL;
}

/* m was just given back (m->owner == NULL): straight to its most urgent
 * waiter (the oldest among equals), who returns from its lock() owning it.
 * Returns that thread, or NULL if nobody waited (m stays free). */
static hermes_thread_t *hand_over(hermes_mutex_t *m) {
    hermes_thread_t *best = NULL;
    for (hermes_thread_t *w = m->waiters.head; w; w = w->queue_next)
        if (!best || HERMES_URG_MORE(HERMES_URG_EFF(w), HERMES_URG_EFF(best))) best = w;
    if (!best) return NULL;
    hermes_sched_wake(best); /* out of the waiters, READY */
    best->blocked_on = NULL;
    best->blocked_kind = HERMES_BLOCK_NONE;
    take(m, best);
    return best;
}

void hermes_mutex_thread_gone(hermes_thread_t *t) {
    while (t->held) {
        hermes_mutex_t *m = t->held;
        untake(m);
        hand_over(m);
    }
}

/* --- public API ------------------------------------------------------------ */

hermes_mutex_t *hermes_mutex_create(void) {
    hermes_mutex_t *m = hermes_runtime_alloc(sizeof(hermes_mutex_t));
    if (!m) return NULL; /* the caller decides, as with hermes_sem_create */
    m->owner = NULL;
    m->next_held = NULL;
    m->waiters.head = NULL;
    m->waiters.tail = NULL;
    return m;
}

void hermes_mutex_destroy(hermes_mutex_t *m) {
    hermes_lock();
    if (m->owner) {
        HERMES_DIAG("hermes: mutex_destroy() on a mutex thread '%s' still holds\n", hermes_device_name(&m->owner->device));
        fatal();
    }
    hermes_unlock();
    hermes_runtime_free(m);
}

void hermes_mutex_lock(hermes_mutex_t *m) {
    hermes_lock();
    hermes_thread_t *self = hermes_thread_current();
    if (!m->owner) {
        take(m, self);
        hermes_unlock();
        return;
    }
    if (m->owner == self) {
        HERMES_DIAG("hermes: mutex_lock() by thread '%s', which already holds it (a mutex isn't "
                    "recursive: it would wait for itself forever)\n", hermes_device_name(&self->device));
        fatal();
    }
    /* following who waits for whom (mutex owners, wait()): does the chain
     * come back to us? */
    int guard = 0;
    for (hermes_thread_t *o = m->owner; o && guard < 64; o = hermes_inherit_next(o), guard++) {
        if (o != self) continue;
        HERMES_DIAG("hermes: deadlock detected (mutex_lock() by thread '%s' (id %d): the mutex's "
                    "owner, thread id %d, waits -- directly or not, through a mutex or wait() -- "
                    "for this one)\n", hermes_device_name(&self->device), self->id, m->owner->id);
        fatal();
    }
    self->blocked_on = m;
    self->blocked_kind = HERMES_BLOCK_MUTEX;
    hermes_prio_recompute(m->owner); /* the owner (and its chain) inherits */
    hermes_sched_block_current(&m->waiters); /* returns, unlocked, owning m (hand_over) */
}

bool hermes_mutex_try_lock(hermes_mutex_t *m) {
    hermes_lock();
    bool got = m->owner == NULL;
    if (got) take(m, hermes_thread_current());
    hermes_unlock();
    return got;
}

void hermes_mutex_unlock(hermes_mutex_t *m) {
    hermes_lock();
    hermes_thread_t *self = hermes_thread_current();
    if (m->owner != self) {
        if (m->owner) {
            HERMES_DIAG("hermes: mutex_unlock() by thread '%s', but thread '%s' holds it (only its "
                        "owner gives a mutex back)\n", hermes_device_name(&self->device), hermes_device_name(&m->owner->device));
        } else {
            HERMES_DIAG("hermes: mutex_unlock() by thread '%s' on a mutex nobody holds\n", hermes_device_name(&self->device));
        }
        fatal();
    }
    untake(m);
    hermes_thread_t *next = hand_over(m);
    /* it no longer inherits from m's waiters; the new owner does */
    hermes_prio_recompute(self);
    /* under the OS: a more urgent thread now has the mutex -- let it run */
    int give_way = next && self->in_process && HERMES_URG_MORE(HERMES_URG_EFF(next), HERMES_URG_EFF(self));
    hermes_unlock();
    if (give_way) hermes_thread_yield();
}
