#include "hermes_sem.h"
#include "hermes_sched_internal.h"

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on bare metal */

/* `count` = tokens available, never negative. A signal with someone
 * waiting hands the token straight to that thread instead of counting it.
 * (Before, count went negative by one per waiter; a thread aborted while
 * waiting -- hermes_thread_abort -- then left its -1 in the count for good.
 * With this form an aborted waiter is simply unlinked and leaves no trace.)
 * Every operation runs under the framework lock: threads on different
 * cores can use the same semaphore.
 *
 * Priority inheritance (decided with the user: declared per semaphore, off
 * by default): `inh` exists once the program asked for it. A semaphore has
 * no owner, so the program says who a waiter waits for:
 *   - sem_inherit_holders(s): the holders -- threads that took a token
 *     (wait / try_wait, or handed one by signal) and haven't given it back
 *     with their own signal. Followed in a table fixed when it's asked
 *     for (decided with the user: no allocation in wait/signal), one slot
 *     per token the semaphore has then (at least 1, at most 255): a holder
 *     beyond that isn't followed;
 *   - sem_signaled_by(s, t): t, the thread that signals it (a producer
 *     that never takes a token).
 * Semaphores with `inh` are in a list, so a thread that ends leaves their
 * tables. */

typedef struct {
    hermes_thread_t *t; /* NULL = free slot */
    unsigned char n;    /* tokens it holds (stops at 255) */
} hermes_sem_hold_t;

typedef struct hermes_sem_inh {
    struct hermes_sem *next_inh;  /* the next semaphore with inheritance */
    hermes_thread_t *signaler;    /* sem_signaled_by; NULL = none */
    unsigned char slots;          /* holders followed; 0 = holders not followed */
    hermes_sem_hold_t holders[];
} hermes_sem_inh_t;

struct hermes_sem {
    long count;
    hermes_wait_queue_t waiters;
    hermes_sem_inh_t *inh;        /* NULL = no inheritance (default) */
};

static hermes_sem_t *g_inh_sems; /* every semaphore with inh */

hermes_sem_t *hermes_sem_create(long initial_count) {
    hermes_sem_t *sem = hermes_runtime_alloc(sizeof(hermes_sem_t));
    if (!sem) {
        return NULL; /* caller's choice how to handle it, unlike hermes_thread_create's fatal error */
    }
    sem->count = initial_count;
    sem->waiters.head = NULL;
    sem->waiters.tail = NULL;
    sem->inh = NULL;
    return sem;
}

/* Lock held. */
static void unlist(hermes_sem_t *sem) {
    hermes_sem_t **link = &g_inh_sems;
    while (*link && *link != sem) link = &(*link)->inh->next_inh;
    if (*link) *link = sem->inh->next_inh;
}

void hermes_sem_destroy(hermes_sem_t *sem) {
    hermes_lock();
    hermes_sem_inh_t *inh = sem->inh;
    if (inh) unlist(sem);
    hermes_unlock();
    if (inh) hermes_runtime_free(inh);
    hermes_runtime_free(sem);
}

/* --- inheritance (lock held) ------------------------------------------------ */

hermes_thread_t *hermes_sem_target(const hermes_sem_t *sem, int k) {
    const hermes_sem_inh_t *inh = sem->inh;
    if (!inh) return NULL;
    for (int i = 0; i < inh->slots; i++)
        if (inh->holders[i].t && k-- == 0) return inh->holders[i].t;
    return k == 0 ? inh->signaler : NULL;
}

/* t took a token. */
static void hold(hermes_sem_inh_t *inh, hermes_thread_t *t) {
    hermes_sem_hold_t *free = NULL;
    for (int i = 0; i < inh->slots; i++) {
        if (inh->holders[i].t == t) {
            if (inh->holders[i].n < 255) inh->holders[i].n++;
            return;
        }
        if (!inh->holders[i].t && !free) free = &inh->holders[i];
    }
    if (free) { free->t = t; free->n = 1; } /* table full: not followed */
}

/* t gave a token back (signal): one fewer, if it held any. */
static void unhold(hermes_sem_inh_t *inh, hermes_thread_t *t) {
    for (int i = 0; i < inh->slots; i++) {
        if (inh->holders[i].t != t) continue;
        if (--inh->holders[i].n == 0) inh->holders[i].t = NULL;
        return;
    }
}

void hermes_sem_thread_gone(hermes_thread_t *t) {
    for (hermes_sem_t *s = g_inh_sems; s; s = s->inh->next_inh) {
        for (int i = 0; i < s->inh->slots; i++)
            if (s->inh->holders[i].t == t) s->inh->holders[i].t = NULL;
        if (s->inh->signaler == t) s->inh->signaler = NULL;
    }
}

/* Lock held exactly once: sem's inh with room for `slots` holders (the
 * ones it had are kept). A fatal error without memory. */
static hermes_sem_inh_t *inh_with(hermes_sem_t *sem, unsigned char slots) {
    hermes_sem_inh_t *old = sem->inh;
    if (old && old->slots >= slots) return old;
    hermes_sem_inh_t *inh = hermes_runtime_alloc(sizeof(hermes_sem_inh_t) + slots * sizeof(hermes_sem_hold_t));
    if (!inh) {
        hermes_unlock();
        HERMES_DIAG("hermes: semaphore inheritance: out of memory\n");
        hermes_fatal();
    }
    for (int i = 0; i < slots; i++) { inh->holders[i].t = NULL; inh->holders[i].n = 0; }
    inh->slots = slots;
    inh->signaler = NULL;
    if (old) {
        inh->signaler = old->signaler;
        for (int i = 0; i < old->slots; i++) inh->holders[i] = old->holders[i];
        unlist(sem);
        hermes_runtime_free(old);
    }
    sem->inh = inh;
    inh->next_inh = g_inh_sems;
    g_inh_sems = sem;
    return inh;
}

void hermes_sem_inherit_holders(hermes_sem_t *sem) {
    hermes_lock();
    long n = sem->count;
    inh_with(sem, (unsigned char)(n < 1 ? 1 : n > 255 ? 255 : n));
    hermes_prio_recompute(NULL);
    hermes_unlock();
}

void hermes_sem_signaled_by(hermes_sem_t *sem, hermes_thread_t *t) {
    hermes_lock();
    hermes_thread_check_live(t, HERMES_PSTR("sem_signaled_by"));
    inh_with(sem, 0)->signaler = t;
    hermes_prio_recompute(t);
    hermes_unlock();
}

/* --- operations --------------------------------------------------------------- */

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
    hermes_thread_t *self = hermes_thread_current();
    if (sem->count > 0) {
        sem->count--;
        if (sem->inh) {
            hold(sem->inh, self);
            if (sem->waiters.head) hermes_prio_recompute(self);
        }
        hermes_unlock();
        return;
    }
    if (sem->inh) {
        /* the holders / the signaler inherit this one's urgency */
        self->blocked_on = sem;
        self->blocked_kind = HERMES_BLOCK_SEM;
        hermes_prio_recompute(self);
    }
    hermes_sched_block_current(&sem->waiters); /* returns, unlocked, once signal() hands it a token */
}

bool hermes_sem_try_wait(hermes_sem_t *sem) {
    hermes_lock();
    bool got = sem->count > 0;
    if (got) {
        sem->count--;
        if (sem->inh) {
            hold(sem->inh, hermes_thread_current());
            if (sem->waiters.head) hermes_prio_recompute(NULL);
        }
    }
    hermes_unlock();
    return got;
}

void hermes_sem_signal(hermes_sem_t *sem) {
    hermes_lock();
    hermes_thread_t *self = hermes_thread_current();
    hermes_thread_t *w = sem->waiters.head;
    if (w) {
        hermes_sched_wake(w); /* the token goes straight to it */
        w->blocked_on = NULL;
        w->blocked_kind = HERMES_BLOCK_NONE;
    } else {
        sem->count++; /* nobody (alive) waiting: keep the token */
    }
    int give_way = 0;
    if (sem->inh) {
        unhold(sem->inh, self);
        if (w) hold(sem->inh, w);
        /* self no longer inherits from w; w inherits from the rest */
        hermes_prio_recompute(self);
        /* under the OS: a more urgent thread got the token -- let it run
         * (as mutex_unlock does) */
        give_way = w && self->in_process && HERMES_URG_MORE(HERMES_URG_EFF(w), HERMES_URG_EFF(self));
    }
    hermes_unlock();
    if (give_way) hermes_thread_yield();
}
