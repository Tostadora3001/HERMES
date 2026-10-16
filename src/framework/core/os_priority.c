#include "os_priority.h"
#include "hermes_os_sched.h"
#include "hermes_sched_internal.h"
#include "hermes_os.h"

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on bare metal */

#ifndef HERMES_OS_PRIORITY
#error "os_priority needs -DHERMES_OS_PRIORITY (hepyc passes it with 'import os_priority')"
#endif

/* One list per priority level (see hermes_os_sched.h for the rules every
 * scheduler follows). Between levels, strict priority. Within a level,
 * the fair share (decided with the user: priorities never change; only a
 * record of use decides among equals):
 *
 *   fs_used   the use of each thread: every ms the tick finds it on a
 *             core (charge) it grows by fs_inc = FS_SCALE / share -- ms
 *             weighted by its share (t.share(n), 1-255, default 1: all
 *             alike). A thread with share 3 grows three times slower, so
 *             it may run three times as long for the same fs_used.
 *   choice    the ready thread of the level with the lowest fs_used runs
 *             (on a tie, the one that has waited longest: list order).
 *   slice     the running thread is looked at again once it has had the
 *             core os_slice_ms() ms (default 10; checked when the
 *             scheduler is asked, every os_sched_ticks() OS ticks): a
 *             ready equal whose fs_used is not higher takes the core.
 *             0 = only when it yields, blocks or ends.
 *   only while asking for the CPU (decided with the user): a thread that
 *             blocked, slept, was paused, is new, or comes from another
 *             level (its effective priority changed, own or inherited)
 *             doesn't bring a debt (nor a credit) for that time: when it's
 *             ready again it's put at the fs_used of the least-used thread
 *             of the level (ready or running), if there is one.
 *   no overflow: once a count passes FS_RENORM the OS's work takes
 *             FS_RENORM / 2 off everyone's (stopping at 0). */
#define OS_PRIORITY_LEVELS 8
#define FS_SCALE  16384u
#define FS_LEVEL  0x07       /* fs_flags: the level it was counted at */
#define FS_STALE  0x08       /* fs_flags: it stopped asking for the CPU */
#define FS_RENORM 0x80000000UL

static hermes_wait_queue_t g_levels[OS_PRIORITY_LEVELS];

#ifndef HERMES_OS_SLICE_MS
#define HERMES_OS_SLICE_MS 10
#endif
static unsigned int g_slice_ms = HERMES_OS_SLICE_MS;
static unsigned long g_since[HERMES_MAX_CORES]; /* when each core's thread got it */
static hermes_thread_t *g_cur[HERMES_MAX_CORES]; /* the thread each core got from pick */
static volatile unsigned char g_renorm;

static int level_of(const hermes_thread_t *t) { return t->priority % OS_PRIORITY_LEVELS; }

static void off_core(const hermes_thread_t *t) {
    for (int c = 0; c < HERMES_MAX_CORES; c++)
        if (g_cur[c] == t) g_cur[c] = NULL;
}

/* The lowest fs_used among the threads of level p but t, ready or
 * running; 0 if there is none. */
static int lowest_use(int p, const hermes_thread_t *t, unsigned long *out) {
    int any = 0;
    for (hermes_thread_t *x = g_levels[p].head; x; x = x->queue_next)
        if (x != t && (!any || x->fs_used < *out)) { *out = x->fs_used; any = 1; }
    for (int c = 0; c < HERMES_MAX_CORES; c++) {
        hermes_thread_t *x = g_cur[c];
        if (x && x != t && level_of(x) == p && (!any || x->fs_used < *out)) { *out = x->fs_used; any = 1; }
    }
    return any;
}

static void created(hermes_thread_t *t) {
    t->fs_used = 0;
    t->fs_inc = FS_SCALE;
    t->fs_flags = FS_STALE;
}

static void ready(hermes_thread_t *t) {
    int p = level_of(t);
    off_core(t); /* it yielded or was taken off: it no longer runs */
    if ((t->fs_flags & FS_STALE) || (t->fs_flags & FS_LEVEL) != p) {
        unsigned long m = 0;
        if (lowest_use(p, t, &m)) t->fs_used = m;
        t->fs_flags = (unsigned char)p;
    }
    hermes_queue_push(&g_levels[p], t);
}

static void left(hermes_thread_t *t) {
    off_core(t);
    t->fs_flags |= FS_STALE;
}

static void charge(hermes_thread_t *t) {
    t->fs_used += t->fs_inc;
    if (t->fs_used >= FS_RENORM) g_renorm = 1;
}

static int due(unsigned long now) {
    (void)now;
    return g_renorm;
}

/* (The OS calls it whenever it works -- e.g. to wake a sleeper -- not
 * only when due() asked.) */
static void work(unsigned long now) {
    (void)now;
    if (!g_renorm) return;
    g_renorm = 0;
    for (hermes_device_t *it = hermes_device_class_thread.head; it; it = it->next) {
        hermes_thread_t *t = (hermes_thread_t *)it;
        if (!t->in_process) continue;
        t->fs_used = t->fs_used > FS_RENORM / 2 ? t->fs_used - FS_RENORM / 2 : 0;
    }
}

static int may_run(const hermes_thread_t *t, int core) {
    return !t->paused && hermes_os_may_run_on(t, core);
}

/* The least-used thread of level p that may run on core and isn't taken
 * (the first one on a tie); NULL if none. */
static hermes_thread_t *least_used(int p, int core, hermes_thread_t *const *taken, int ntaken) {
    hermes_thread_t *best = NULL;
    for (hermes_thread_t *t = g_levels[p].head; t; t = t->queue_next) {
        if (!may_run(t, core) || hermes_os_is_taken(t, taken, ntaken)) continue;
        if (!best || t->fs_used < best->fs_used) best = t;
    }
    return best;
}

/* Most urgent level first; within it, the least used. */
static hermes_thread_t *pick(int core) {
    for (int p = OS_PRIORITY_LEVELS - 1; p >= 0; p--) {
        hermes_thread_t *t = least_used(p, core, NULL, 0);
        if (!t) continue;
        hermes_queue_unlink(t);
        g_since[core] = hermes_os_now_ms(); /* its slice starts */
        g_cur[core] = t;
        return t;
    }
    return NULL;
}

/* The tick: a ready thread of HIGHER priority takes the core from the
 * running one; one of EQUAL priority, once the running one has had its
 * slice (os_slice_ms) and if it hasn't used more than the running one
 * (fair share). A running thread that may no longer stay here (paused,
 * or migrated / pinned to another core) leaves. */
static hermes_thread_t *decide(int core, hermes_thread_t *running,
                               hermes_thread_t *const *taken, int ntaken) {
    if (!may_run(running, core)) return NULL;
    int level = level_of(running);
    for (int p = OS_PRIORITY_LEVELS - 1; p > level; p--) {
        hermes_thread_t *t = least_used(p, core, taken, ntaken);
        if (t) return t;
    }
    if (g_slice_ms && hermes_os_now_ms() - g_since[core] >= g_slice_ms) {
        hermes_thread_t *t = least_used(level, core, taken, ntaken);
        if (t && t->fs_used <= running->fs_used) return t;
    }
    return running;
}

/* t.share(n) (os_priority.h). */
void hermes_os_share(hermes_thread_t *t, long n) {
    hermes_lock();
    hermes_thread_check_live(t, HERMES_PSTR("share"));
    if (n < 1 || n > 255) {
        HERMES_DIAG("hermes_os: share(%ld): must be between 1 and 255\n", n);
        hermes_unlock();
        hermes_fatal();
    }
    t->fs_inc = (unsigned short)((FS_SCALE + (unsigned long)n / 2) / (unsigned long)n);
    hermes_unlock();
}

/* t.priority(p) (hermes_os.h). */
void hermes_os_set_priority(hermes_thread_t *t, long p) {
    hermes_lock();
    hermes_thread_check_live(t, HERMES_PSTR("priority"));
    if (p < 0 || p > 7) {
        HERMES_DIAG("hermes_os: priority(%ld): must be between 0 and 7\n", p);
        hermes_unlock();
        hermes_fatal();
    }
    t->base_priority = (unsigned char)p;
    /* the effective one: p, or more while it holds a mutex a more urgent
     * thread waits for; re-filed in the ready set if it changes */
    hermes_prio_recompute(t);
    hermes_sched_notify();
    hermes_unlock();
}

/* os_slice_ms(ms): only while the OS is stopped, as os_tick_ms. */
void os_slice_ms(long ms) {
    hermes_lock();
    if (hermes_os_hooks->running) {
        HERMES_DIAG("hermes_os: os_slice_ms() while the OS is running: call it before os_run()\n");
        hermes_unlock();
        hermes_fatal();
    }
    if (ms < 0 || ms > 65535) {
        HERMES_DIAG("hermes_os: os_slice_ms(%ld): must be between 0 (no slicing) and 65535\n", ms);
        hermes_unlock();
        hermes_fatal();
    }
    g_slice_ms = (unsigned int)ms;
    hermes_unlock();
}

const hermes_os_scheduler_t hermes_os_scheduler HERMES_SCHED_TABLE = {
    .name = "priority", .ready = ready, .pick = pick, .decide = decide,
    .charge = charge, .left = left, .created = created, .due = due, .work = work,
};
