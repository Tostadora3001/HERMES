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
 * Two tiers, per the project's design (see CLAUDE.md and
 * docs/raw/hermes_threads.en.md):
 *   - automatic: thread_start() + threads_run() -- sensible defaults, the
 *     scheduler manages everything from there.
 *   - concrete: thread_start_ex() with an explicit heap/stack split, plus
 *     direct access to thread_join()/thread_yield() and the semaphore
 *     primitives for hand-written synchronization over shared resources.
 *
 * Naming note: sem_wait/sem_signal/sem_destroy intentionally reuse names
 * that also exist in POSIX <semaphore.h>. This header never includes
 * <semaphore.h> and HERMES's own semaphores are not POSIX semaphores --
 * do not mix the two in the same C translation unit.
 */

#include "hermes_thread.h"
#include "hermes_sem.h"

typedef hermes_thread_t *hermes_thread_handle_t;
typedef hermes_sem_t *hermes_sem_handle_t;

/* --- automatic tier ------------------------------------------------- */

static inline hermes_thread_handle_t thread_start(hermes_thread_fn fn, long arg) {
    return hermes_thread_create(fn, arg);
}

static inline void threads_run(void) {
    hermes_threads_run();
}

/* The thread's identity is assigned automatically by the runtime when it is
 * created (see the g_next_id counter in hermes_thread.c) -- it is NOT
 * something the caller picks. Do not (ab)use thread_start's `arg` parameter
 * as a stand-in identifier: two threads can legitimately be started with
 * the same `arg` (it's just user data passed to the entry function), and
 * that must not be confused with "same thread". Use thread_id()/
 * thread_self_id() whenever code needs to tell threads apart. */
static inline long thread_id(hermes_thread_handle_t t) {
    return (long)t->id;
}

static inline long thread_self_id(void) {
    return (long)hermes_thread_current()->id;
}

/* --- concrete / fine-grained tier ------------------------------------ */

static inline hermes_thread_handle_t thread_start_ex(hermes_thread_fn fn, long arg,
                                                       long heap_size, long stack_size) {
    hermes_thread_attr_t attr;
    attr.heap_size = (size_t)heap_size;
    attr.stack_size = (size_t)stack_size;
    attr.name = NULL;
    return hermes_thread_create_ex(fn, arg, &attr);
}

static inline void thread_join(hermes_thread_handle_t t) {
    hermes_thread_join(t);
}

static inline void thread_yield(void) {
    hermes_thread_yield();
}

/* --- semaphores (both tiers) ----------------------------------------- */

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

#endif
