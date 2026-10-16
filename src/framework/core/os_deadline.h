#ifndef OS_DEADLINE_H
#define OS_DEADLINE_H

/* `import os_deadline`: the earliest-deadline-first (EDF) scheduler of the
 * OS library (hermes_os), for periodic threads. Requires `import
 * hermes_os` written by the user. See docs/raw/hermes_os.en.md.
 *
 * Every thread of the process is periodic (decided with the user: no
 * thread without a deadline). Each period it does one job and calls
 * os_next_period(), which sleeps until the next period starts; the job
 * must be done by its deadline (a time relative to the start of the
 * period). The ready thread whose job's deadline comes first runs; on a
 * tie, the one waiting longest. A ready thread with an earlier deadline
 * takes the core from the running one at the OS tick (preemption on bare
 * metal; the next safe point on the host), as os_priority does with a
 * higher priority. Mutexes pass deadlines on: the owner of a mutex a
 * thread with an earlier deadline waits for runs with that deadline.
 *
 *   t.period(ms)        its period, 1-65535 ms (required: os_run() is a
 *                       fatal error while a thread has none). A thread
 *                       created by a process thread while the OS runs
 *                       (e.g. a parallel for's) takes over its creator's
 *                       period, deadline, on_miss and current period.
 *   t.deadline(ms)      its relative deadline, 1..period (default: the
 *                       period)
 *   os_next_period()    from the thread: this period's job is done, sleep
 *                       until the next one starts (right away if it
 *                       already started: the thread is late)
 *   t.on_miss_warn()    a missed deadline prints a warning (default) ...
 *   t.on_miss_abort()   ... or is a fatal error (the whole program stops,
 *                       every device aborted, as with any fatal error)
 *   t.missed()          deadlines it has missed so far
 *   t.cost(ms)          its cost: the longest a job takes, 1..deadline
 *                       (the worst case, measured or estimated by the
 *                       user). Only for os_feasible(); the OS watches it:
 *                       a job found on a core more ms than its cost is a
 *                       warning (once per job; counted by the 1 ms tick,
 *                       so +-1 ms)
 *   os_feasible()       True if every deadline can be met with the
 *                       declared costs (EDF feasibility test; exact on
 *                       one core, only sufficient on several -- see
 *                       os_deadline.c). Only tells: the program decides
 *                       what to do. Every thread needs period and cost
 *
 * The first period of every thread starts when os_run() starts (or, for
 * a thread given its period while the OS runs, right then); periods come
 * one after another with no drift, whether a job is late or not -- a late
 * job never makes the next ones start later. A job counts as missed if
 * at its deadline it hasn't called os_next_period() yet (seen at the
 * next OS tick), and once only. A new period or deadline set while the
 * thread already has one applies from its next period. Priorities don't
 * exist with it: t.priority(p) is a fatal error. */

#include "hermes_thread.h"
#include <stdbool.h>

#ifndef HERMES_OS_DEADLINE
#error "os_deadline needs -DHERMES_OS_DEADLINE (hepyc passes it with 'import os_deadline')"
#endif

void hermes_dl_period(hermes_thread_t *t, long ms);
void hermes_dl_deadline(hermes_thread_t *t, long ms);
void hermes_dl_on_miss(hermes_thread_t *t, int abort);
long hermes_dl_missed(hermes_thread_t *t);
void hermes_dl_cost(hermes_thread_t *t, long ms);
void os_next_period(void);
bool os_feasible(void);

/* methods on a thread (`device t = thread_start(...)`) ... */
static inline void device_thread_period(hermes_thread_handle_t h_t, long ms) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "period");
    hermes_dl_period(t, ms);
}
static inline void device_thread_deadline(hermes_thread_handle_t h_t, long ms) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "deadline");
    hermes_dl_deadline(t, ms);
}
static inline void device_thread_on_miss_warn(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "on_miss_warn");
    hermes_dl_on_miss(t, 0);
}
static inline void device_thread_on_miss_abort(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "on_miss_abort");
    hermes_dl_on_miss(t, 1);
}
static inline long device_thread_missed(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "missed");
    return hermes_dl_missed(t);
}
static inline void device_thread_cost(hermes_thread_handle_t h_t, long ms) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "cost");
    hermes_dl_cost(t, ms);
}

/* ... and the same as functions (for hermes_thread_handle_t values) */
static inline void thread_period(hermes_thread_handle_t h_t, long ms) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "period");
    hermes_dl_period(t, ms);
}
static inline void thread_deadline(hermes_thread_handle_t h_t, long ms) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "deadline");
    hermes_dl_deadline(t, ms);
}
static inline void thread_on_miss_warn(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "on_miss_warn");
    hermes_dl_on_miss(t, 0);
}
static inline void thread_on_miss_abort(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "on_miss_abort");
    hermes_dl_on_miss(t, 1);
}
static inline long thread_missed(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "missed");
    return hermes_dl_missed(t);
}
static inline void thread_cost(hermes_thread_handle_t h_t, long ms) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "cost");
    hermes_dl_cost(t, ms);
}

#endif
