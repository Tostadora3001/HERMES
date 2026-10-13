#include "os_priority.h"
#include "hermes_os_sched.h"
#include "hermes_sched_internal.h"

/* One FIFO per priority level (see hermes_os_sched.h for the rules every
 * scheduler follows). */
#define OS_PRIORITY_LEVELS 8

static hermes_wait_queue_t g_levels[OS_PRIORITY_LEVELS];

static void ready(hermes_thread_t *t) {
    hermes_queue_push(&g_levels[t->priority % OS_PRIORITY_LEVELS], t);
}

/* Most urgent level first; within a level, the oldest that may run on
 * this core (pin / migrate). */
static hermes_thread_t *pick(int core) {
    for (int p = OS_PRIORITY_LEVELS - 1; p >= 0; p--) {
        for (hermes_thread_t *t = g_levels[p].head; t; t = t->queue_next) {
            if (t->paused || !hermes_os_may_run_on(t, core)) continue;
            hermes_queue_unlink(t);
            return t;
        }
    }
    return NULL;
}

/* The tick: only a ready thread of HIGHER priority takes the core from
 * the running one (decided with the user); equal priorities still take
 * turns only when they yield. A running thread that may no longer stay
 * here (paused, or migrated / pinned to another core) leaves. */
static hermes_thread_t *decide(int core, hermes_thread_t *running,
                               hermes_thread_t *const *taken, int ntaken) {
    if (running->paused || !hermes_os_may_run_on(running, core)) return NULL;
    for (int p = OS_PRIORITY_LEVELS - 1; p > running->priority % OS_PRIORITY_LEVELS; p--) {
        for (hermes_thread_t *t = g_levels[p].head; t; t = t->queue_next) {
            if (t->paused || !hermes_os_may_run_on(t, core) || hermes_os_is_taken(t, taken, ntaken)) continue;
            return t;
        }
    }
    return running;
}

const hermes_os_scheduler_t hermes_os_scheduler = { "priority", ready, pick, decide };
