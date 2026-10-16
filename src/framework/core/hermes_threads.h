#ifndef HERMES_THREADS_H
#define HERMES_THREADS_H

/* HERMESPY-facing threading API. `import hermes_threads` in a .hepy file
 * maps to `#include "hermes_threads.h"`.
 *
 * This is a thin, header-only wrapper around the real implementation in
 * hermes_thread.{h,c} and hermes_sem.{h,c}, renamed/retyped so every symbol
 * fits HERMESPY v0.1's grammar: a "type" usable from .hepy must be a single
 * identifier, so every pointer/opaque handle gets a one-word typedef below
 * (no `*` ever appears in a .hepy type name).
 *
 * There is NO scheduler here (see docs/raw/hermes_threads.en.md): creating
 * a thread doesn't run it. The program decides who runs and when --
 * t.run() on its own core, t.launch()/t.launch_on(n) on another one -- and
 * waits for it with t.wait(). Every operation exists both as a method
 * (`t.run()`, for `device t = thread_start(...)`) and as a plain function
 * (`thread_run(t)`, for a `hermes_thread_handle_t` variable or parameter).
 *
 * Naming note: sem_wait/sem_signal/sem_destroy intentionally reuse names
 * that also exist in POSIX <semaphore.h>. This header never includes
 * <semaphore.h> and HERMES's own semaphores are not POSIX semaphores --
 * do not mix the two in the same C translation unit.
 */

#include "hermes_thread.h"
#include "hermes_sem.h"
#include "hermes_mutex.h"
#include "hermes_device_thread.h"

typedef hermes_sem_t *hermes_sem_handle_t;
typedef hermes_mutex_t *hermes_mutex_handle_t;

/* --- creating ------------------------------------------------------- */

/* Default memory sizes (per target, see hermes_thread.h). */
static inline hermes_thread_handle_t thread_start(hermes_thread_fn fn, long arg) {
    return hermes_device_handle(&hermes_thread_create(fn, arg)->device);
}

/* Explicit heap/stack split. */
static inline hermes_thread_handle_t thread_start_ex(hermes_thread_fn fn, long arg,
                                                       long heap_size, long stack_size) {
    hermes_thread_attr_t attr;
    attr.heap_size = (size_t)heap_size;
    attr.stack_size = (size_t)stack_size;
    return hermes_device_handle(&hermes_thread_create_ex(fn, arg, &attr)->device);
}

/* The thread's identity is assigned automatically by the runtime when it is
 * created (see the g_next_id counter in hermes_thread.c) -- it is NOT
 * something the caller picks. Do not (ab)use thread_start's `arg` parameter
 * as a stand-in identifier: two threads can legitimately be started with
 * the same `arg` (it's just user data passed to the entry function), and
 * that must not be confused with "same thread". Use thread_id()/
 * thread_self_id() whenever code needs to tell threads apart. */
static inline long thread_id(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "id");
    return (long)hermes_thread_id(t);
}

static inline long thread_self_id(void) {
    return (long)hermes_thread_current()->id;
}

/* --- running and waiting -------------------------------------------- */

static inline void thread_run(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "run");
    hermes_thread_run(t);
}
static inline void thread_launch(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "launch");
    hermes_thread_launch(t);
}
static inline void thread_launch_on(hermes_thread_handle_t h_t, long core) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "launch_on");
    hermes_thread_launch_on(t, (int)core);
}
static inline void thread_wait(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "wait");
    hermes_thread_wait(t);
}
static inline void thread_join(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "join");
    hermes_thread_wait(t);
} /* old name */
static inline bool thread_done(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "done");
    return hermes_thread_done(t) != 0;
}

/* Give control back to whoever ran this thread (no-op on main). */
static inline void thread_yield(void) {
    hermes_thread_yield();
}

/* --- stopping -------------------------------------------------------- */

static inline void thread_pause(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "pause");
    hermes_thread_pause(t);
}
static inline void thread_resume(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "resume");
    hermes_thread_resume(t);
}
static inline void thread_abort(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "abort");
    hermes_thread_abort(t);
}
static inline void thread_destroy(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "destroy");
    hermes_thread_destroy(t);
}

/* --- cores ----------------------------------------------------------- */

static inline long cores_count(void) { return hermes_cores_count(); }
static inline long cores_free(void) { return hermes_cores_free(); }
static inline long core_id(void) { return hermes_core_current(); }

/* --- semaphores ------------------------------------------------------ */

static inline hermes_sem_handle_t sem_create(long initial_count) {
    return hermes_sem_create(initial_count);
}

static inline void sem_wait(hermes_sem_handle_t s) {
    hermes_sem_wait(s);
}

static inline void sem_signal(hermes_sem_handle_t s) {
    hermes_sem_signal(s);
}

static inline bool sem_try_wait(hermes_sem_handle_t s) {
    return hermes_sem_try_wait(s);
}

static inline void sem_destroy(hermes_sem_handle_t s) {
    hermes_sem_destroy(s);
}

/* priority inheritance through a semaphore (under hermes_os; see
 * hermes_sem.h): the holders of its tokens, and/or the thread that
 * signals it */
static inline void sem_inherit_holders(hermes_sem_handle_t s) {
    hermes_sem_inherit_holders(s);
}

static inline void sem_signaled_by(hermes_sem_handle_t s, hermes_thread_handle_t h_t) {
    hermes_sem_signaled_by(s, HERMES_THREAD_GET(h_t, "sem_signaled_by"));
}

/* --- mutexes (an owner; priority inheritance under hermes_os) -------- */

static inline hermes_mutex_handle_t mutex_create(void) { return hermes_mutex_create(); }
static inline void mutex_lock(hermes_mutex_handle_t m) { hermes_mutex_lock(m); }
static inline bool mutex_try_lock(hermes_mutex_handle_t m) { return hermes_mutex_try_lock(m); }
static inline void mutex_unlock(hermes_mutex_handle_t m) { hermes_mutex_unlock(m); }
static inline void mutex_destroy(hermes_mutex_handle_t m) { hermes_mutex_destroy(m); }

#endif
