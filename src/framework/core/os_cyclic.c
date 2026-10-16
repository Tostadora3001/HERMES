#include "os_cyclic.h"
#include "hermes_os_sched.h"
#include "hermes_sched_internal.h"
#include "hermes_os.h"

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on bare metal */

/* See os_cyclic.h for the policy and hermes_os_sched.h for the rules every
 * scheduler follows. The order is the creation order -- the thread ids,
 * which only grow -- so the ready set needs no order of its own: pick()
 * takes the ready thread that comes next after the last one it chose. */

static hermes_wait_queue_t g_ready;
static int g_last; /* id of the last thread picked (0 = none yet) */

static void ready(hermes_thread_t *t) {
    hermes_queue_push(&g_ready, t);
}

static int may_run(const hermes_thread_t *t, int core) {
    return !t->paused && hermes_os_may_run_on(t, core);
}

/* The first one after g_last in the order; past the end, back to the
 * start (the lowest id). */
static hermes_thread_t *pick(int core) {
    hermes_thread_t *next = NULL, *first = NULL;
    for (hermes_thread_t *t = g_ready.head; t; t = t->queue_next) {
        if (!may_run(t, core)) continue;
        if (t->id > g_last && (!next || t->id < next->id)) next = t;
        if (!first || t->id < first->id) first = t;
    }
    if (!next) next = first;
    if (!next) return NULL;
    hermes_queue_unlink(next);
    g_last = next->id;
    return next;
}

/* The tick never takes the core away: only a thread that may no longer
 * stay here (paused, or migrated / pinned to another core) leaves. */
static hermes_thread_t *decide(int core, hermes_thread_t *running,
                               hermes_thread_t *const *taken, int ntaken) {
    (void)taken; (void)ntaken;
    return may_run(running, core) ? running : NULL;
}

/* t.priority(p) (hermes_os.h): no priorities here. */
void hermes_os_set_priority(hermes_thread_t *t, long p) {
    (void)t; (void)p;
    HERMES_DIAG("hermes_os: priority(): os_cyclic has no priorities (threads take turns in "
                "creation order)\n");
    hermes_fatal();
}

/* Every os_run() starts the round from the first thread. */
static void start(void) { g_last = 0; }

const hermes_os_scheduler_t hermes_os_scheduler HERMES_SCHED_TABLE = {
    .name = "cyclic", .ready = ready, .pick = pick, .decide = decide,
    .start = start,
};
