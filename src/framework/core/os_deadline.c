#include "os_deadline.h"
#include "hermes_os_sched.h"
#include "hermes_sched_internal.h"
#include "hermes_os.h"

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on bare metal */

/* See os_deadline.h for the policy and hermes_os_sched.h for the rules
 * every scheduler follows. Two lists: the ready threads (in arrival order:
 * on a tie of deadlines the first one wins) and the parked ones -- ready
 * but without a period yet, so they can't run. Deadlines are compared on
 * dl_eff, the effective one (mutex inheritance); misses on dl_abs, the
 * thread's own job. */

#define HERMES_DL_ABORT  0x01 /* on_miss_abort() */
#define HERMES_DL_MISSED 0x02 /* the current job already counted as missed */
#define HERMES_DL_OVER   0x04 /* the current job already went over its cost */
#define HERMES_DL_OVER_NEW 0x08 /* ... and the warning is still to be given */

static hermes_wait_queue_t g_ready, g_parked;
/* the earliest deadline among the jobs not counted as missed yet (g_watch
 * = there is one): the tick asks for work when it passes */
static unsigned long g_check;
static unsigned char g_watch;
/* a job went over its declared cost: work() has a warning to give */
static volatile unsigned char g_over;

/* Every caller holds the lock exactly once. */
static void fatal(void) {
    hermes_unlock();
    hermes_fatal();
}

static int earlier(unsigned long a, unsigned long b) { return (long)(a - b) < 0; }

static unsigned int rel_deadline(const hermes_thread_t *t) {
    return t->dl_rel ? t->dl_rel : t->dl_period;
}

static void watch(unsigned long at) {
    if (!g_watch || earlier(at, g_check)) g_check = at;
    g_watch = 1;
}

/* Lock held: t's period (and its job) starts at `at`. */
static void begin_period(hermes_thread_t *t, unsigned long at) {
    t->dl_release = at;
    t->dl_abs = at + rel_deadline(t);
    t->dl_used = 0;
    t->dl_flags &= (unsigned char)~(HERMES_DL_MISSED | HERMES_DL_OVER);
    hermes_prio_recompute(t); /* dl_eff: dl_abs, or an inherited earlier one */
    watch(t->dl_abs);
}

/* --- the scheduler interface ----------------------------------------------- */

/* A thread created by a periodic thread while the OS runs (a parallel
 * for's threads, or any other) takes over its creator's period, deadline
 * and on_miss -- and its current period: it works for the same job, with
 * the same deadline. Otherwise it would have no period and could never
 * run, and its creator, waiting for it, would never finish. */
static void created(hermes_thread_t *t) {
    hermes_thread_t *by = hermes_thread_current();
    if (!hermes_os_hooks->running || !by->in_process || !by->dl_period) return;
    t->dl_period = by->dl_period;
    t->dl_rel = by->dl_rel;
    t->dl_cost = by->dl_cost;
    t->dl_flags = by->dl_flags & HERMES_DL_ABORT;
    t->dl_release = by->dl_release;
    t->dl_abs = by->dl_abs;
    hermes_prio_recompute(t);
    watch(t->dl_abs);
}

static void ready(hermes_thread_t *t) {
    hermes_queue_push(t->dl_period ? &g_ready : &g_parked, t);
}

static int may_run(const hermes_thread_t *t, int core) {
    return !t->paused && hermes_os_may_run_on(t, core);
}

/* The earliest effective deadline that may run on this core (the first
 * one on a tie). */
static hermes_thread_t *pick(int core) {
    hermes_thread_t *best = NULL;
    for (hermes_thread_t *t = g_ready.head; t; t = t->queue_next)
        if (may_run(t, core) && (!best || earlier(t->dl_eff, best->dl_eff))) best = t;
    if (best) hermes_queue_unlink(best);
    return best;
}

/* The tick: a ready thread with an EARLIER deadline takes the core (an
 * equal one waits until the running one yields), as os_priority does with
 * a higher priority. A running thread that may no longer stay here
 * (paused, or migrated / pinned to another core) leaves. */
static hermes_thread_t *decide(int core, hermes_thread_t *running,
                               hermes_thread_t *const *taken, int ntaken) {
    if (!may_run(running, core)) return NULL;
    hermes_thread_t *best = running;
    for (hermes_thread_t *t = g_ready.head; t; t = t->queue_next)
        if (may_run(t, core) && !hermes_os_is_taken(t, taken, ntaken) && earlier(t->dl_eff, best->dl_eff))
            best = t;
    return best;
}

/* os_run() starts: every period starts now. A thread without a period is
 * a fatal error (decided with the user: no thread without a deadline). */
static void start(void) {
    unsigned long now = hermes_os_now_ms();
    g_watch = 0;
    for (hermes_device_t *it = hermes_device_class_thread.head; it; it = it->next) {
        hermes_thread_t *t = (hermes_thread_t *)it;
        if (!t->in_process || t->state == HERMES_THREAD_TERMINATED) continue;
        if (!t->dl_period) {
            HERMES_DIAG("hermes_os: thread '%s' (id %d) has no period: with os_deadline every thread "
                        "needs one (t.period(ms)) before os_run()\n", hermes_device_name(&t->device), t->id);
            fatal();
        }
        begin_period(t, now);
    }
}

/* From the tick, every ms: one more ms used by the running thread's job.
 * Over its declared cost (once per job): a warning, which work() gives. */
static void charge(hermes_thread_t *t) {
    if (!t->dl_cost || (t->dl_flags & HERMES_DL_OVER)) return;
    if (++t->dl_used > t->dl_cost) {
        t->dl_flags |= HERMES_DL_OVER | HERMES_DL_OVER_NEW;
        g_over = 1;
    }
}

static int due(unsigned long now) {
    return g_over || (g_watch && (long)(now - g_check) > 0);
}

/* Lock held exactly once: the job of t missed its deadline. Counted once
 * per job; the message goes out with the lock released (on bare metal the
 * lock masks interrupts, and a line on the UART takes milliseconds). On
 * return the lock is held again -- unless it was on_miss_abort(). */
static void missed(hermes_thread_t *t) {
    t->dl_flags |= HERMES_DL_MISSED;
    if (t->dl_missed < 65535) t->dl_missed++;
    const char *name = hermes_device_name(&t->device);
    int id = t->id;
    unsigned int rel = rel_deadline(t);
    int abort = t->dl_flags & HERMES_DL_ABORT;
    hermes_unlock();
    if (abort) {
        HERMES_DIAG("hermes_os: thread '%s' (id %d) missed its deadline (%u ms after the start of its "
                    "period) and it's on_miss_abort(): stopping\n", name, id, rel);
        hermes_fatal();
    }
    HERMES_DIAG("hermes_os: warning: thread '%s' (id %d) missed its deadline (%u ms after the start "
                "of its period)\n", name, id, rel);
    (void)name; (void)id; (void)rel;
    hermes_lock();
}

/* The OS's work (the tick saw a deadline pass): every job past its
 * deadline that hasn't called os_next_period() is a miss; then the next
 * deadline to watch. */
static void work(unsigned long now) {
    /* jobs that went over their declared cost (charge()): one warning
     * each, the lock released while it goes out */
    while (g_over) {
        g_over = 0;
        for (hermes_device_t *it = hermes_device_class_thread.head; it; it = it->next) {
            hermes_thread_t *t = (hermes_thread_t *)it;
            if (!(t->dl_flags & HERMES_DL_OVER_NEW)) continue;
            t->dl_flags &= (unsigned char)~HERMES_DL_OVER_NEW;
            const char *name = hermes_device_name(&t->device);
            int id = t->id;
            unsigned int cost = t->dl_cost;
            hermes_unlock();
            HERMES_DIAG("hermes_os: warning: thread '%s' (id %d) used more than its cost (%u ms) in a "
                        "job: os_feasible() no longer holds\n", name, id, cost);
            (void)name; (void)id; (void)cost;
            hermes_lock();
            g_over = 1; /* the list may have changed meanwhile: look again */
            break;
        }
    }
    for (;;) {
        hermes_thread_t *late = NULL;
        g_watch = 0;
        for (hermes_device_t *it = hermes_device_class_thread.head; it; it = it->next) {
            hermes_thread_t *t = (hermes_thread_t *)it;
            if (!t->in_process || t->state == HERMES_THREAD_TERMINATED || !t->dl_period ||
                (t->dl_flags & HERMES_DL_MISSED))
                continue;
            if ((long)(now - t->dl_abs) > 0) { if (!late) late = t; }
            else watch(t->dl_abs);
        }
        if (!late) return;
        missed(late); /* releases the lock meanwhile: look again afterwards */
    }
}

const hermes_os_scheduler_t hermes_os_scheduler HERMES_SCHED_TABLE = {
    .name = "deadline", .ready = ready, .pick = pick, .decide = decide,
    .start = start, .charge = charge, .created = created, .due = due, .work = work,
};

/* --- HERMESPY-facing ---------------------------------------------------------- */

/* t.priority(p) (hermes_os.h): no priorities here. */
void hermes_os_set_priority(hermes_thread_t *t, long p) {
    (void)t; (void)p;
    HERMES_DIAG("hermes_os: priority(): os_deadline has no priorities (the earliest deadline runs)\n");
    hermes_fatal();
}

static void check_ms(long ms, const char *op) {
    if (ms >= 1 && ms <= 65535) return;
    HERMES_DIAG("hermes_os: %" HERMES_PS "(%ld): must be between 1 and 65535 ms\n", op, ms);
    (void)op;
    fatal();
}

void hermes_dl_period(hermes_thread_t *t, long ms) {
    hermes_lock();
    hermes_thread_check_live(t, HERMES_PSTR("period"));
    check_ms(ms, HERMES_PSTR("period"));
    if (t->dl_rel > ms) {
        HERMES_DIAG("hermes_os: period(%ld) shorter than the thread's deadline (%u ms)\n", ms,
                    (unsigned int)t->dl_rel);
        fatal();
    }
    if (t->dl_cost > ms) {
        HERMES_DIAG("hermes_os: period(%ld) shorter than the thread's cost (%u ms)\n", ms,
                    (unsigned int)t->dl_cost);
        fatal();
    }
    int first = !t->dl_period;
    t->dl_period = (unsigned short)ms;
    if (first && t->in_process && t->state != HERMES_THREAD_TERMINATED) {
        /* it can run now: its first period starts now if the OS runs (if
         * not, os_run() starts it) */
        if (hermes_os_hooks->running) begin_period(t, hermes_os_now_ms());
        if (t->queue == &g_parked) {
            hermes_queue_unlink(t);
            hermes_queue_push(&g_ready, t);
        }
        hermes_sched_notify();
    }
    hermes_unlock();
}

void hermes_dl_deadline(hermes_thread_t *t, long ms) {
    hermes_lock();
    hermes_thread_check_live(t, HERMES_PSTR("deadline"));
    check_ms(ms, HERMES_PSTR("deadline"));
    if (t->dl_period && ms > t->dl_period) {
        HERMES_DIAG("hermes_os: deadline(%ld) longer than the thread's period (%u ms)\n", ms,
                    (unsigned int)t->dl_period);
        fatal();
    }
    if (t->dl_cost > ms) {
        HERMES_DIAG("hermes_os: deadline(%ld) shorter than the thread's cost (%u ms)\n", ms,
                    (unsigned int)t->dl_cost);
        fatal();
    }
    t->dl_rel = (unsigned short)ms;
    hermes_unlock();
}

void hermes_dl_cost(hermes_thread_t *t, long ms) {
    hermes_lock();
    hermes_thread_check_live(t, HERMES_PSTR("cost"));
    check_ms(ms, HERMES_PSTR("cost"));
    unsigned int limit = t->dl_rel ? t->dl_rel : t->dl_period;
    if (limit && ms > (long)limit) {
        HERMES_DIAG("hermes_os: cost(%ld) longer than the thread's deadline (%u ms)\n", ms, limit);
        fatal();
    }
    t->dl_cost = (unsigned short)ms;
    hermes_unlock();
}

void hermes_dl_on_miss(hermes_thread_t *t, int abort) {
    hermes_lock();
    hermes_thread_check_live(t, abort ? HERMES_PSTR("on_miss_abort") : HERMES_PSTR("on_miss_warn"));
    if (abort) t->dl_flags |= HERMES_DL_ABORT;
    else t->dl_flags &= (unsigned char)~HERMES_DL_ABORT;
    hermes_unlock();
}

long hermes_dl_missed(hermes_thread_t *t) {
    hermes_lock();
    hermes_thread_check_live(t, HERMES_PSTR("missed"));
    long n = t->dl_missed;
    hermes_unlock();
    return n;
}

/* This period's job is done: a miss if its deadline already passed (and
 * the tick hasn't seen it yet); then the next period, which starts exactly
 * one period after this one (no drift); sleep until then -- or, if it
 * already started, go on at once (only giving the others their chance). */
void os_next_period(void) {
    hermes_lock();
    hermes_thread_t *self = hermes_thread_current();
    if (!self->in_process || !hermes_os_hooks->running) {
        HERMES_DIAG("hermes_os: os_next_period() only from a thread of the process while the OS runs\n");
        fatal();
    }
    if (!(self->dl_flags & HERMES_DL_MISSED) && (long)(hermes_os_now_ms() - self->dl_abs) > 0) missed(self);
    unsigned long next = self->dl_release + self->dl_period;
    begin_period(self, next);
    if ((long)(hermes_os_now_ms() - next) >= 0) {
        hermes_unlock();
        hermes_thread_yield();
        return;
    }
    hermes_os_sleep_until_locked(next); /* returns, unlocked, at the start of the period */
}

/* --- os_feasible(): can every deadline be met? --------------------------------
 * The test is done on what the threads declare: cost C (t.cost), relative
 * deadline D and period T, for every periodic thread of the process (a
 * thread without a period or a cost is a fatal error: there's nothing to
 * test). Assumed, as in every EDF test: independent jobs (time blocked on
 * a mutex or a semaphore isn't counted) and the OS's own time (tick,
 * interrupts, print() on the UART) is free -- leave room for it. The OS
 * watches the cost (charge()): a job that goes over it is a warning.
 *
 * One core (bare metal, or a host with one): exact. Every period starts at
 * os_run() (the synchronous case), and EDF meets every deadline if and
 * only if
 *   - U = sum C/T <= 1 (checked exactly: sum C*(H/T) <= H, H = the
 *     hyperperiod, lcm of the periods), and
 *   - if some D < T, at every absolute deadline L within the first busy
 *     period, the demand -- sum, over the jobs with release and deadline
 *     in [0, L], of their cost -- is at most L (processor demand
 *     criterion; the busy period is the fixed point of w = sum
 *     ceil(w/T)*C, at most H when U <= 1).
 * All in 32-bit integers (no float on AVR). A hyperperiod above
 * DL_TEST_MAX ms (~24 days) can't be checked: False.
 *
 * Several cores (host): only sufficient tests, so False may also mean "not
 * proven". Every thread pinned (t.pin): each core on its own, with the
 * exact test (partitioned EDF). No thread pinned: global EDF with the
 * density test of Goossens, Funk and Baruah, sum C/D <= m - (m-1)*max C/D
 * (m = cores). Some pinned and some not: False (the free ones can take a
 * pinned one's core: no known test covers it). And the host isn't real
 * time: the test can't promise what Linux doesn't. */

typedef struct { unsigned int c, d, t; signed char pin; } dl_task_t;

#define DL_TEST_MAX 0x7FFE0000UL

static unsigned long gcd_ul(unsigned long a, unsigned long b) {
    while (b) { unsigned long r = a % b; a = b; b = r; }
    return a;
}

/* The exact one-core test on the tasks with pin == core (core < 0: all). */
static bool feasible_one(const dl_task_t *k, int n, int core) {
    unsigned long h = 1;
    int implicit = 1, any = 0;
    for (int i = 0; i < n; i++) {
        if (core >= 0 && k[i].pin != core) continue;
        any = 1;
        unsigned long g = gcd_ul(h, k[i].t);
        if (h / g > DL_TEST_MAX / k[i].t) return false; /* hyperperiod too long */
        h = h / g * k[i].t;
        if (k[i].d != k[i].t) implicit = 0;
    }
    if (!any) return true;
    /* U <= 1, exactly: every term C*(H/T) <= H, as C <= T */
    unsigned long sum = 0;
    for (int i = 0; i < n; i++) {
        if (core >= 0 && k[i].pin != core) continue;
        unsigned long term = k[i].c * (h / k[i].t);
        if (term > h - sum) return false;
        sum += term;
    }
    if (implicit) return true; /* D = T: U <= 1 is enough */
    /* the first busy period (U <= 1: it converges, at most at H; no
     * iterate passes it, so nothing overflows) */
    unsigned long w = 0;
    for (int i = 0; i < n; i++)
        if (core < 0 || k[i].pin == core) w += k[i].c;
    for (;;) {
        unsigned long next = 0;
        for (int i = 0; i < n; i++)
            if (core < 0 || k[i].pin == core) next += (w + k[i].t - 1) / k[i].t * k[i].c;
        if (next == w) break;
        w = next;
    }
    /* the demand at every absolute deadline in it */
    for (int i = 0; i < n; i++) {
        if (core >= 0 && k[i].pin != core) continue;
        for (unsigned long l = k[i].d; l <= w; l += k[i].t) {
            unsigned long demand = 0;
            for (int j = 0; j < n; j++) {
                if ((core >= 0 && k[j].pin != core) || k[j].d > l) continue;
                unsigned long term = ((l - k[j].d) / k[j].t + 1) * k[j].c; /* <= l, as C <= D */
                if (term > l - demand) return false;
                demand += term;
            }
        }
    }
    return true;
}

#if HERMES_MAX_CORES > 1
static bool feasible_many(const dl_task_t *k, int n, int m) {
    int pinned = 0;
    for (int i = 0; i < n; i++) pinned += k[i].pin >= 0;
    if (pinned == n) {
        for (int core = 0; core < m; core++)
            if (!feasible_one(k, n, core)) return false;
        return true;
    }
    if (pinned) return false;
    double density = 0, max = 0;
    for (int i = 0; i < n; i++) {
        double di = (double)k[i].c / k[i].d;
        density += di;
        if (di > max) max = di;
    }
    return density <= m - (m - 1) * max;
}
#endif

bool os_feasible(void) {
    hermes_lock();
    int n = 0;
    for (hermes_device_t *it = hermes_device_class_thread.head; it; it = it->next) {
        hermes_thread_t *t = (hermes_thread_t *)it;
        if (t->in_process && t->state != HERMES_THREAD_TERMINATED) n++;
    }
    if (n == 0) { hermes_unlock(); return true; }
    dl_task_t k[n]; /* copied, so the test runs with the lock released */
    int i = 0;
    for (hermes_device_t *it = hermes_device_class_thread.head; it && i < n; it = it->next) {
        hermes_thread_t *t = (hermes_thread_t *)it;
        if (!t->in_process || t->state == HERMES_THREAD_TERMINATED) continue;
        if (!t->dl_period || !t->dl_cost) {
            HERMES_DIAG("hermes_os: os_feasible(): thread '%s' (id %d) has no %" HERMES_PS
                        ": the test needs t.period(ms) and t.cost(ms) of every thread\n",
                        hermes_device_name(&t->device), t->id, !t->dl_period ? HERMES_PSTR("period") : HERMES_PSTR("cost"));
            fatal();
        }
        k[i].c = t->dl_cost;
        k[i].d = rel_deadline(t);
        k[i].t = t->dl_period;
        k[i].pin = t->pin;
        i++;
    }
    n = i;
    int cores = hermes_cores_count();
    hermes_unlock();
#if HERMES_MAX_CORES > 1
    if (cores > 1) return feasible_many(k, n, cores);
#endif
    (void)cores;
    return feasible_one(k, n, -1);
}
