#ifndef HERMES_THREAD_H
#define HERMES_THREAD_H

#include <stddef.h>
#include "hermes_platform.h"
#include "hermes_ctx.h"
#include "hermes_device.h"

/* Thread library: the MECHANISM to create, run, launch on other cores,
 * stop, pause and destroy threads -- deliberately NO scheduler (no policy
 * deciding which thread runs next). Who runs when is the user's decision
 * (or, later, the OS library's). See docs/raw/hermes_threads.en.md.
 *
 *   t.run()          runs `t` on the calling core, like a coroutine: it
 *                    comes back to the caller when `t` yields, blocks on a
 *                    semaphore or finishes.
 *   t.launch()       hands `t` to the first free core (another physical
 *                    core: a pthread on the host); if none is free it waits
 *                    there for one ("waiting for resources").
 *   t.launch_on(n)   same, on core n (1..cores-1; 0 is main's core).
 *   t.wait()         waits, without running anything, until `t` finishes.
 *
 * A thread is bound to the first core that runs it until it finishes (no
 * migration). Any operation on a destroyed thread (or NULL) is a fatal
 * error. */

/* Device class shared by every thread (defined in hermes_thread.c). */
extern hermes_device_class_t hermes_device_class_thread;

typedef enum {
    HERMES_THREAD_READY,      /* can be run (never started, or yielded) */
    HERMES_THREAD_RUNNING,    /* executing, or suspended inside a t.run()
                                 of its own (it is on its core's chain) */
    HERMES_THREAD_BLOCKED,    /* waiting on a semaphore, a mutex, another
                                 thread, or asleep (delay_ms under hermes_os) */
    HERMES_THREAD_TERMINATED  /* finished or aborted, for good */
} hermes_thread_state_t;

/* Thread entry points take a single `long` argument. HERMESPY v0.1 has no
 * address-of operator and no raw pointer types, so a `long` (rather than
 * `void *`) is what a .hepy program can actually construct and pass; see
 * docs/raw/hermes_threads.en.md. */
typedef void (*hermes_thread_fn)(long arg);

/* Where a thread waits: a semaphore's waiters or a core's launch queue. */
typedef struct {
    struct hermes_thread *head;
    struct hermes_thread *tail;
} hermes_wait_queue_t;

/* --- per-thread memory block --------------------------------------------
 *
 *   +-----------------------------+  <- block_start (lowest address)
 *   | start canary                |  HERMES_CANARY_BYTES of a fixed pattern
 *   +-----------------------------+
 *   | hermes_thread_t (the TCB)   |  key thread data, inside its own block
 *   +-----------------------------+
 *   | heap area (bump allocator)  |  grows upward -- hermes_thread_alloc()
 *   +-----------------------------+
 *   | stack area                  |  grows downward, used by the CPU/ctx
 *   +-----------------------------+
 *   | end canary                  |  same pattern
 *   +-----------------------------+  <- block_start + block_size (highest)
 *
 * The canaries are checked every time the thread gives control back
 * (yield, block, finish) and before destroy releases the block: a changed
 * canary means something wrote outside the thread's memory -> fatal error
 * naming the thread. Detection, not prevention: by the time it's seen, the
 * memory has already been overwritten. */
#if HERMES_BARE_METAL
#define HERMES_CANARY_BYTES 4   /* one 32-bit pattern (AVR, RV32) */
#else
#define HERMES_CANARY_BYTES 8   /* one 64-bit pattern; keeps the TCB aligned */
#endif
typedef struct hermes_thread {
    hermes_device_t device; /* must be first -- makes a hermes_thread_t* a
                                valid hermes_device_t* (see hermes_device.h);
                                its name is the kind's ("thread"; "main"):
                                hermes_device_name */
    hermes_ctx_t ctx;
    hermes_thread_state_t state;
    hermes_thread_fn entry;
    long arg;
    int id;

    /* true from creation until destroy(). Every operation on a thread
     * checks it first (using a destroyed thread is a fatal error). Placed
     * past the first 32 bytes of the block on purpose: free() writes its
     * own bookkeeping at the start of a freed block (glibc up to 32 B,
     * avr-libc 2 B on AVR), so a flag there wouldn't
     * read as false after destroy. */
    unsigned char alive;

    unsigned char *block_start;
    size_t block_size;
    unsigned char *heap_cursor; /* next free byte in the heap area */
    unsigned char *heap_limit;  /* first byte NOT available to the heap
                                    (== first byte of the stack area) */

    /* Whoever ran this thread (t.run(), or a core's own loop): where
     * control goes back when it yields, blocks or finishes. NULL while
     * not running. */
    struct hermes_thread *resumer;

    /* Linkage reused for whichever single queue this thread currently sits
     * in (`queue`): one semaphore's waiters or one launch queue. A thread
     * is never in two at once, so one field suffices. */
    struct hermes_thread *queue_next;
    hermes_wait_queue_t *queue;

    signed char core;       /* bound core, -1 = not bound yet */
    unsigned char paused;   /* pause(): nobody resumes it until resume() */
    unsigned char on_cpu;   /* its context is live on some core: its
                               memory can't be released yet */
    unsigned char is_root;  /* main, or a core's own base context: never
                               switched away from, has no resumer */

    /* Threads waiting for this one to finish while blocked (a process
     * thread's wait() under hermes_os); woken when it terminates. */
    hermes_wait_queue_t joiners;

    /* Used only under hermes_os (the OS library, hermes_os.c):
     * in_process = its block lives in the process and the scheduler runs
     * it; priority 0-7 (higher = more urgent, default 4); pin = only ever
     * on that core (-1 = any); next_core = on that core next time only
     * (-1 = any). */
    unsigned char in_process;
    unsigned char priority;      /* effective: base_priority, or more while it
                                    holds a mutex a more urgent thread wants */
    unsigned char base_priority; /* the one set with t.priority(p) */
#ifndef HERMES_OS_DEADLINE
    unsigned char prio_new;      /* scratch of hermes_inherit.c */
#endif
    signed char pin;
    signed char next_core;
    /* os_preempt_off() nesting (and print() on bare metal): while > 0 the
     * OS never takes the core from it (hermes_preempt_off/on). */
    unsigned char no_preempt;

    /* Priority inheritance (hermes_inherit.c): what it's blocked on --
     * blocked_kind HERMES_BLOCK_MUTEX (a hermes_mutex_t), _THREAD (a
     * hermes_thread_t: wait()) or _SEM (a hermes_sem_t with inheritance);
     * HERMES_BLOCK_NONE (0) = nothing it can pass urgency through -- and
     * the list of the mutexes it holds. */
    void *blocked_on;
    unsigned char blocked_kind;
    struct hermes_mutex *held;

    /* delay_ms under hermes_os: asleep until the OS's ms counter reaches
     * this (hermes_os.c). */
    unsigned long wake_at;

#ifdef HERMES_OS_PRIORITY
    /* os_priority (hepyc passes -DHERMES_OS_PRIORITY with the import; no
     * cost otherwise): the use record of the fair share among equal
     * priorities (os_priority.c). fs_used grows by fs_inc (16384 / share)
     * every ms the tick finds it on a core; the level it was counted at
     * and whether it stopped asking for the CPU go in fs_flags. */
    unsigned long fs_used;
    unsigned short fs_inc;
    unsigned char fs_flags;
#endif

#ifdef HERMES_OS_DEADLINE
    /* os_deadline (hepyc passes -DHERMES_OS_DEADLINE with the import; no
     * cost otherwise): every process thread is periodic. Times in the
     * OS's ms (hermes_os.c). dl_abs = the deadline of the current job
     * (period start + relative deadline); dl_eff = dl_abs, or earlier
     * while it holds a mutex a thread with an earlier deadline waits for
     * (inheritance: the mutex code orders by it, see HERMES_URG_* in
     * hermes_sched_internal.h). */
    unsigned long dl_release;  /* start of the current period */
    unsigned long dl_abs;
    unsigned long dl_eff;
    unsigned long dl_new;      /* scratch of hermes_inherit.c */
    unsigned short dl_period;  /* ms; 0 = none yet (it can't run) */
    unsigned short dl_rel;     /* relative deadline, ms; 0 = the period */
    unsigned short dl_missed;  /* deadlines missed (stops at 65535) */
    unsigned short dl_cost;    /* declared cost of a job, ms; 0 = none */
    unsigned short dl_used;    /* ms the tick found it on a core, this job */
    unsigned char dl_flags;    /* HERMES_DL_* (os_deadline.c) */
#endif

    /* (No list of all threads here: every thread, main included, is in
     * the thread kind's device list -- hermes_device_class_thread.) */
} hermes_thread_t;

/* heap_size value for a thread with no heap of its own (all its block is
 * stack) -- used by parallel for, whose threads never use one. */
#define HERMES_THREAD_NO_HEAP ((size_t)-1)

typedef struct {
    size_t heap_size;  /* 0 => HERMES_THREAD_DEFAULT_HEAP_SIZE;
                          HERMES_THREAD_NO_HEAP => none */
    size_t stack_size; /* 0 => HERMES_THREAD_DEFAULT_STACK_SIZE */
} hermes_thread_attr_t;

/* Default block of thread_start()/hermes_thread_create() and of every
 * parallel for thread, chosen by the thread-size profile import (hepyc
 * passes -DHERMES_THREAD_PROFILE: 1 light, 2 medium, 3 heavy; without one,
 * medium). Per target: the host needs far bigger stacks -- a thread that
 * calls print() (glibc printf) crashed with 1536 B of stack and worked
 * with 2048 B (measured).
 *
 *              AVR (heap + stack)      host (heap + stack)
 *   light       32 +  256               256 +  2 KB
 *   medium      64 +  512               512 +  8 KB
 *   heavy      128 + 1024              1 KB + 16 KB
 *
 * HERMES_THREAD_DEFAULT_HEAP_SIZE / _STACK_SIZE can still be forced
 * with -D. */
#ifndef HERMES_THREAD_PROFILE
#define HERMES_THREAD_PROFILE 2
#endif
#if HERMES_THREAD_PROFILE < 1 || HERMES_THREAD_PROFILE > 3
#error "HERMES_THREAD_PROFILE: 1 (light), 2 (medium) or 3 (heavy)"
#endif
#if defined(__AVR__)
#define HERMES_THREAD_PROFILE_HEAP  (HERMES_THREAD_PROFILE == 1 ? 32ul  : HERMES_THREAD_PROFILE == 2 ? 64ul  : 128ul)
#define HERMES_THREAD_PROFILE_STACK (HERMES_THREAD_PROFILE == 1 ? 256ul : HERMES_THREAD_PROFILE == 2 ? 512ul : 1024ul)
#elif HERMES_BARE_METAL
/* RISC-V microcontrollers (RV32): 4-byte registers and a 64-byte switch
 * frame, print() without printf (hermes_stdlib.h) -- twice AVR's sizes
 * leave the same margin; the ESP32-C3 has ~320 KB of RAM for them. */
#define HERMES_THREAD_PROFILE_HEAP  (HERMES_THREAD_PROFILE == 1 ? 64ul  : HERMES_THREAD_PROFILE == 2 ? 128ul  : 256ul)
#define HERMES_THREAD_PROFILE_STACK (HERMES_THREAD_PROFILE == 1 ? 512ul : HERMES_THREAD_PROFILE == 2 ? 1024ul : 2048ul)
#else
#define HERMES_THREAD_PROFILE_HEAP  (HERMES_THREAD_PROFILE == 1 ? 256ul  : HERMES_THREAD_PROFILE == 2 ? 512ul  : 1024ul)
#define HERMES_THREAD_PROFILE_STACK (HERMES_THREAD_PROFILE == 1 ? 2048ul : HERMES_THREAD_PROFILE == 2 ? 8192ul : 16384ul)
#endif
#ifndef HERMES_THREAD_DEFAULT_HEAP_SIZE
#define HERMES_THREAD_DEFAULT_HEAP_SIZE  HERMES_THREAD_PROFILE_HEAP
#endif
#ifndef HERMES_THREAD_DEFAULT_STACK_SIZE
#define HERMES_THREAD_DEFAULT_STACK_SIZE HERMES_THREAD_PROFILE_STACK
#endif

/* Creates a thread, READY and not bound to any core. It doesn't run until
 * someone runs or launches it. */
hermes_thread_t *hermes_thread_create(hermes_thread_fn entry, long arg);

/* Same, with an explicit heap/stack split and name. */
hermes_thread_t *hermes_thread_create_ex(hermes_thread_fn entry, long arg,
                                          const hermes_thread_attr_t *attr);

/* Bump-allocates `size` bytes from thread `t`'s own heap area. Returns NULL
 * if `t`'s heap area is exhausted. */
void *hermes_thread_alloc(hermes_thread_t *t, size_t size);

/* Runs `t` on the calling core until it yields, blocks or finishes.
 * No-op if `t` can't run now: finished, paused, blocked, already running,
 * bound to another core, or the caller itself. A thread waiting in a
 * launch queue is taken out of it and run here. */
void hermes_thread_run(hermes_thread_t *t);

/* Queues `t` for the first free core other than main's. It waits there
 * if every core is busy. Never runs it on the calling core. No-op if `t`
 * already started or is already queued. */
void hermes_thread_launch(hermes_thread_t *t);

/* Queues `t` on core `core` (1..hermes_cores_count()-1). Fatal error if
 * that core doesn't exist (core 0 is main's: use run()). */
void hermes_thread_launch_on(hermes_thread_t *t, int core);

/* Waits until `t` has finished. It runs nothing: a thread nobody runs or
 * launches is never waited for -- if no core can make progress any more
 * that's a deadlock, reported as a fatal error. No-op on the caller itself
 * and on main. */
void hermes_thread_wait(hermes_thread_t *t);

int hermes_thread_done(hermes_thread_t *t);

/* Cooperative: a running thread stops at its next yield/block point. */
void hermes_thread_pause(hermes_thread_t *t);
void hermes_thread_resume(hermes_thread_t *t);

/* Gives control back to whoever ran the calling thread. No-op on main (and
 * on any root context: there's no one to give control to). */
void hermes_thread_yield(void);

/* Stops `t` for good: it never runs again and threads waiting for it go
 * on. On the calling thread itself it doesn't return (like the thread's
 * function returning). If `t` is executing on another core right now it
 * stops at its next yield/block point. No-op on `main` and on an already
 * terminated thread. Its memory is not released (see destroy). */
void hermes_thread_abort(hermes_thread_t *t);

/* Aborts `t`, waits until its context is no longer live on any core,
 * removes it from the device list, zeroes its control block and releases
 * its memory. Any later operation on `t` is a fatal error (every operation
 * checks the thread is still in the list); hepyc also sets the variable
 * used in `t.destroy()` to NULL. On the calling thread itself, or on a thread that
 * this one is running inside of (its chain of run() calls), it can't
 * release the memory still in use: it only aborts. No-op on main. */
void hermes_thread_destroy(hermes_thread_t *t);

hermes_thread_t *hermes_thread_current(void);

/* The thread's unique id (assigned at creation; main is 0). */
int hermes_thread_id(hermes_thread_t *t);

/* Physical cores this program can use (host: hardware threads; AVR: 1) and
 * the one the caller runs on (0 = main's). */
int hermes_cores_count(void);
int hermes_core_current(void);

/* Cores with nothing to run right now, counting the caller's own core
 * (the caller is free to work itself). Always >= 1. */
int hermes_cores_free(void);

/* What HERMESPY holds for a thread: a handle (slot + generation, see
 * "handles" in hermes_device.h), not a pointer. The HERMESPY-facing
 * functions (hermes_threads.h, hermes_device_thread.h, the OS's headers)
 * take one and resolve it first: a destroyed thread's handle -- a copy of
 * it, even if a new thread now lives in the same memory -- or 0 is a
 * fatal error with its message. Inside the framework, threads are
 * pointers (h = hermes_device_handle(&t->device)). */
typedef hermes_handle_t hermes_thread_handle_t;
#define HERMES_THREAD_GET(h, method)                                                \
    ((hermes_thread_t *)hermes_handle_get((h), &hermes_device_class_thread,         \
                                          HERMES_PSTR("thread"), HERMES_PSTR(method)))

#endif
