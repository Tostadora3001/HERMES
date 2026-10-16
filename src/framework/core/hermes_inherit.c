#include "hermes_sched_internal.h"

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on bare metal */

/* Priority inheritance: one engine for every kind of wait (see
 * hermes_sched_internal.h and "Shared resources and priorities" in
 * docs/raw/hermes_os.en.md).
 *
 * A blocked thread says what it waits for (blocked_on / blocked_kind):
 *   a mutex      -> its owner;
 *   a thread     -> that thread (wait());
 *   a semaphore  -> the threads it passes urgency to, if the program
 *                   asked for it (sem_inherit_holders: the threads holding
 *                   tokens; sem_signaled_by: the thread that signals it).
 * Those are the threads it "waits for". Every thread runs at least as
 * urgently as anyone waiting for it, directly or through a chain of
 * waits: that's its effective urgency (priority, or deadline with
 * os_deadline: HERMES_URG_*). Its own one is the base.
 *
 * Computed from scratch every time (hermes_prio_recompute): each process
 * thread starts at its base and the urgency of every waiter flows to the
 * threads it waits for until nothing changes -- a fixed point over the
 * graph of waits (in a scratch field of each TCB, HERMES_URG_NEW). From scratch, not step by step, because a semaphore
 * fans out to several threads and its waits may form a cycle (allowed:
 * someone outside may still signal it), and a stepwise update could leave
 * an urgency circling in a cycle after whoever brought it has gone. It
 * reaches the fixed point in at most as many rounds as threads. Cost:
 * O(rounds x threads x what each waits for); with no waiting thread, one
 * pass. Only process threads count (the OS's priorities mean
 * nothing elsewhere); this file is linked only with hermes_os. */

/* The k-th thread w waits for (NULL: no more). */
static hermes_thread_t *target(const hermes_thread_t *w, int k) {
    if (w->blocked_kind == HERMES_BLOCK_SEM) return hermes_sem_target(w->blocked_on, k);
    return k == 0 ? hermes_inherit_next(w) : NULL;
}

static int counts(const hermes_thread_t *t) {
    return t->in_process && t->state != HERMES_THREAD_TERMINATED;
}

void hermes_inherit_recompute(void) {
    if (!hermes_os_hooks) return;
    hermes_device_t *const all = hermes_device_class_thread.head;
    int n = 0, waiting = 0;
    for (hermes_device_t *it = all; it; it = it->next) {
        hermes_thread_t *x = (hermes_thread_t *)it;
        HERMES_URG_NEW(x) = HERMES_URG_BASE(x);
        if (!counts(x)) continue;
        n++;
        waiting |= x->blocked_kind != HERMES_BLOCK_NONE;
    }
    /* the waiters' urgency flows to whom they wait for, round after round
     * (at most n: the longest chain) until nothing changes */
    for (int round = 0, changed = waiting; changed && round <= n; round++) {
        changed = 0;
        for (hermes_device_t *it = all; it; it = it->next) {
            hermes_thread_t *w = (hermes_thread_t *)it, *x;
            if (w->blocked_kind == HERMES_BLOCK_NONE || !counts(w)) continue;
            for (int k = 0; (x = target(w, k)) != NULL; k++) {
                if (counts(x) && HERMES_URG_MORE(HERMES_URG_NEW(w), HERMES_URG_NEW(x))) {
                    HERMES_URG_NEW(x) = HERMES_URG_NEW(w);
                    changed = 1;
                }
            }
        }
    }
    for (hermes_device_t *it = all; it; it = it->next) {
        hermes_thread_t *x = (hermes_thread_t *)it;
        if (!counts(x) || HERMES_URG_NEW(x) == HERMES_URG_EFF(x)) continue;
        HERMES_URG_EFF(x) = HERMES_URG_NEW(x);
        hermes_os_hooks->reprioritized(x);
    }
}

void hermes_inherit_wait_check(hermes_thread_t *self, hermes_thread_t *t) {
    int guard = 0;
    for (hermes_thread_t *x = t; x && guard < 64; x = hermes_inherit_next(x), guard++) {
        if (x != self) continue;
        HERMES_DIAG("hermes: deadlock detected (wait() by thread '%s' (id %d) for thread id %d, "
                    "which waits -- directly or not, through wait() or a mutex -- for this one)\n",
                    hermes_device_name(&self->device), self->id, t->id);
        hermes_unlock();
        hermes_fatal();
    }
}
