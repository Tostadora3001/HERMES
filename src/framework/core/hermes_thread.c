#include "hermes_platform.h" /* HERMES_BARE_METAL: macros only */
#if !HERMES_BARE_METAL
#define _XOPEN_SOURCE 700 /* PTHREAD_MUTEX_RECURSIVE, clock_gettime */
#endif
#include "hermes_thread.h"
#include "hermes_sched_internal.h"
#include "hermes_ctx.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on bare metal */

/* --- cores ----------------------------------------------------------------
 * A "core" is something that executes hermes threads at the same time as
 * the others. Core 0 is main's. On the host each other core is a pthread
 * (one per hardware thread, started on the first launch); bare metal
 * (AVR, RISC-V microcontrollers) has only core 0. Per-core state (which thread is running here) is thread-local
 * on the host: a hermes thread is bound to one core, so it always runs on
 * the same pthread. */
#if HERMES_BARE_METAL
/* one core: plain globals */
#define HERMES_TLS_VAR(type, name, init) static type name##_tls = init; \
    static inline type *name##_addr(void) { return &name##_tls; }
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
/* Per-core state is thread-local (one pthread per core). Under hermes_os a
 * hermes thread can continue on another core after a context switch, so
 * the compiler must never keep a TLS address computed before a switch: it
 * assumes the thread pointer (%fs) never changes inside a function and may
 * cache it in a register. Every access to the runtime's TLS goes through a
 * noinline/noipa accessor instead -- an opaque call, re-done each time.
 * HERMES_TLS_VAR(type, name, init) defines `name` as such an accessor. */
#define HERMES_TLS_VAR(type, name, init)                                       \
    static _Thread_local type name##_tls = init;                               \
    __attribute__((noinline, noipa)) static type *name##_addr(void) {          \
        return &name##_tls;                                                    \
    }
#endif

/* --- the framework lock (see hermes_sched_internal.h) ---------------------
 * Besides the lock: `notify` tells every core waiting in idle_wait that
 * something changed, and `g_active` counts the cores not waiting -- when
 * it reaches 0 and nothing changes any more, nobody can wake anybody: a
 * deadlock, reported instead of hanging silently. */
HERMES_TLS_VAR(int, g_lock_depth, 0)
#define g_lock_depth (*g_lock_depth_addr())
static unsigned long g_epoch = 0; /* bumped by every notify */
static void deadlock(const char *what);
static void switch_point(void);

#if HERMES_BARE_METAL

#if defined(__AVR__)
#include <avr/io.h>
#include <avr/interrupt.h>
#endif

/* --- interrupts off/on: the framework lock on one core --------------------
 * irq_save() masks interrupts and returns what to give irq_restore(). */
#if defined(__AVR__)
typedef unsigned char hermes_irq_state_t;
static inline hermes_irq_state_t irq_save(void) { unsigned char s = SREG; cli(); return s; }
static inline void irq_restore(hermes_irq_state_t s) { SREG = s; }
static inline int irq_enabled(void) { return (SREG & (1 << SREG_I)) != 0; }
#elif defined(__riscv)
typedef unsigned long hermes_irq_state_t;
/* mstatus.MIE (bit 3): machine-mode interrupt enable */
static inline hermes_irq_state_t irq_save(void) {
    unsigned long s;
    __asm__ __volatile__("csrrci %0, mstatus, 8" : "=r"(s) :: "memory");
    return s & 8;
}
static inline void irq_restore(hermes_irq_state_t s) {
    __asm__ __volatile__("csrs mstatus, %0" :: "r"(s) : "memory");
}
static inline int irq_enabled(void) {
    unsigned long s;
    __asm__ __volatile__("csrr %0, mstatus" : "=r"(s));
    return (s & 8) != 0;
}
#else
#error "hermes_thread.c: bare-metal target without irq_save/irq_restore"
#endif

static hermes_irq_state_t g_lock_irq;

/* --- main's stack boundary (bare metal) ------------------------------------
 * main() has no thread block: it runs on the MCU's own stack, which grows
 * down from the end of RAM. The heap (thread blocks, devices) may not go
 * above the start of main's stack, HERMES_MAIN_STACK_BYTES below the end
 * of RAM (see hermes_runtime_alloc). A canary sits right below that
 * boundary -- the heap ends below the canary, so malloc never uses it. If
 * main's stack grows past its size it overwrites the canary first; it's
 * checked every time the framework lock is taken (any thread, semaphore,
 * device or allocation operation), and a broken one is a fatal error.
 * (The host doesn't need it: the OS puts a guard page under main's
 * stack, so overflowing it is an immediate crash, not silent corruption.)
 *
 *   AVR: computed from RAMEND here, and avr-libc's malloc is told where
 *        the heap ends (__malloc_heap_end, set when the canary is armed).
 *   RISC-V: the platform's linker script places it (symbols
 *        __hermes_main_canary and __hermes_main_stack_bytes) and ends
 *        picolibc's heap there (__heap_end). */
#define HERMES_MAIN_CANARY_BYTES 4
#if defined(__AVR__)
#ifndef HERMES_AVR_MAIN_STACK_BYTES
#define HERMES_AVR_MAIN_STACK_BYTES 512
#endif
#define HERMES_MAIN_STACK_BYTES HERMES_AVR_MAIN_STACK_BYTES
#define HERMES_MAIN_CANARY_AT \
    ((volatile unsigned char *)(RAMEND + 1 - HERMES_AVR_MAIN_STACK_BYTES - HERMES_MAIN_CANARY_BYTES))
extern char *__malloc_heap_end;
#else
extern unsigned char __hermes_main_canary[];
extern unsigned char __hermes_main_stack_bytes[]; /* absolute symbol: its address is the size */
#define HERMES_MAIN_STACK_BYTES ((int)(unsigned long)__hermes_main_stack_bytes)
#define HERMES_MAIN_CANARY_AT ((volatile unsigned char *)__hermes_main_canary)
#endif
static const unsigned char HERMES_MAIN_CANARY[HERMES_MAIN_CANARY_BYTES] = { 0xC4, 0x9A, 0x2E, 0x51 };
static unsigned char g_main_canary_armed = 0;

/* At startup (hermes_scheduler_bootstrap, before main()): the stack is
 * still a few bytes deep, far above the boundary. */
static void main_canary_arm(void) {
#if defined(__AVR__)
    __malloc_heap_end = (char *)HERMES_MAIN_CANARY_AT;
#endif
    for (unsigned char i = 0; i < HERMES_MAIN_CANARY_BYTES; i++) HERMES_MAIN_CANARY_AT[i] = HERMES_MAIN_CANARY[i];
    g_main_canary_armed = 1;
}

static void main_canary_check(void) {
    for (unsigned char i = 0; i < HERMES_MAIN_CANARY_BYTES; i++) {
        if (HERMES_MAIN_CANARY_AT[i] == HERMES_MAIN_CANARY[i]) continue;
        g_main_canary_armed = 0; /* the fatal path takes the lock again */
        while (g_lock_depth > 0) hermes_unlock();
        HERMES_DIAG("hermes: main's stack went past its %d B (boundary canary overwritten)\n",
                    HERMES_MAIN_STACK_BYTES);
        hermes_fatal();
    }
}

void hermes_lock(void) {
    if (g_lock_depth == 0) switch_point();
    hermes_irq_state_t s = irq_save();
    if (g_lock_depth++ == 0) {
        g_lock_irq = s;
        if (g_main_canary_armed) main_canary_check();
    }
}

void hermes_unlock(void) {
    if (--g_lock_depth == 0) irq_restore(g_lock_irq);
}

static void notify_locked(void) { g_epoch++; }
static void notify_work_locked(void) { g_epoch++; }

/* One core: if it has to wait, nothing else can ever make the change it
 * waits for (ISRs don't touch threads). */
static void idle_wait(void) { deadlock(HERMES_PSTR("the only core is waiting for something nobody can do")); }

#else

static pthread_mutex_t g_mutex;
/* Two conditions so that a change of some thread's state doesn't wake
 * every idle core: g_cond for whoever waits on a thread (wait, a core
 * driving its thread, a blocked main), g_work_cond for idle cores waiting
 * for something to be launched. */
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_work_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_lock_once = PTHREAD_ONCE_INIT;
static int g_active = 1; /* main's core */

static void lock_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void hermes_lock(void) {
    if (g_lock_depth == 0) switch_point();
    pthread_once(&g_lock_once, lock_init);
    pthread_mutex_lock(&g_mutex);
    g_lock_depth++;
}

void hermes_unlock(void) {
    g_lock_depth--;
    pthread_mutex_unlock(&g_mutex);
}

static void notify_locked(void) {
    g_epoch++;
    pthread_cond_broadcast(&g_cond);
}

static void notify_work_locked(void) {
    g_epoch++;
    pthread_cond_broadcast(&g_work_cond);
    pthread_cond_broadcast(&g_cond);
}

/* Called with the lock held exactly once; returns after some notify (the
 * caller re-checks what it waits for). The last core to go idle waits
 * with a timeout: if every core is still idle and nothing changed, it's a
 * deadlock. */
static void idle_wait_on(pthread_cond_t *cond) {
    unsigned long seen = g_epoch;
    g_lock_depth--; /* the cond wait releases the mutex */
    g_active--;
    if (g_active == 0) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 200000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        int r = 0;
        while (g_epoch == seen && r != ETIMEDOUT) r = pthread_cond_timedwait(cond, &g_mutex, &until);
        if (g_epoch == seen && g_active == 0) {
            g_active++;
            g_lock_depth++;
            deadlock(HERMES_PSTR("every core is waiting and none can go on"));
        }
    } else {
        while (g_epoch == seen) pthread_cond_wait(cond, &g_mutex);
    }
    g_active++;
    g_lock_depth++;
}

static void idle_wait(void) { idle_wait_on(&g_cond); }

#endif

/* --- hermes_runtime_alloc/free: single allocation point for the framework
 * (declared in hermes_sched_internal.h): malloc/free, thread-safe. Memory
 * comes from whatever is free -- there is no fixed budget (the profile
 * imports only choose the default size of each thread's block). */
/* (Bare metal: the heap ends below main's stack canary, see above.)
 * AVR: avr-libc's malloc, when __malloc_heap_end is 0 (the default),
 * takes "current SP - __malloc_margin" as the heap's upper limit. From
 * inside a hermes thread the SP lives in that thread's own block -- i.e.
 * in the heap itself -- so malloc concluded there was no room: every
 * device/thread created from a thread aborted (found when the UART driver
 * first made AVR output visible). So the limit is fixed at startup, right
 * below main's stack canary (main_canary_arm): independent of which stack
 * is current. */
void *hermes_runtime_alloc(size_t n) {
    hermes_lock();
    void *p = malloc(n);
    hermes_unlock();
    return p;
}

void hermes_runtime_free(void *p) {
    hermes_lock();
    free(p);
    hermes_unlock();
}


/* The thread's device class (see hermes_device.h): the one and only
 * definition, so its address is the single identity of "a thread"; it
 * also holds the list of every thread. No hardware limit. */
static void thread_abort_entry(hermes_device_t *d);
hermes_device_class_t hermes_device_class_thread = {
    .name = "thread", .max_instances = 0, .abort = thread_abort_entry
};

static hermes_thread_t g_main_thread;
HERMES_TLS_VAR(hermes_thread_t *, g_current, NULL)
#define g_current (*g_current_addr())
HERMES_TLS_VAR(int, g_core_index, 0)
#define g_core_index (*g_core_index_addr())
static int g_next_id = 1;

/* Threads launched with launch() wait here for any free core. */
static hermes_wait_queue_t g_launch_any;

#if !HERMES_BARE_METAL
typedef struct {
    hermes_thread_t base;      /* the core's own context (its pthread) */
    hermes_wait_queue_t queue; /* threads launched with launch_on(k) */
    unsigned char busy;        /* running a thread right now */
} hermes_core_t;

static hermes_core_t g_cores[HERMES_MAX_CORES]; /* [0] unused: main's core */
static int g_core_count = 1;
static int g_workers_started = 0;
#endif

static int is_root_ptr(hermes_thread_t *t) {
    if (t == &g_main_thread) return 1;
#if !HERMES_BARE_METAL
    if ((void *)t >= (void *)&g_cores[0] && (void *)t < (void *)&g_cores[HERMES_MAX_CORES]) return 1;
#endif
    return 0;
}

/* --- queues --------------------------------------------------------------- */

static void queue_push(hermes_wait_queue_t *q, hermes_thread_t *t) {
    t->queue_next = NULL;
    t->queue = q;
    if (q->tail) q->tail->queue_next = t;
    else q->head = t;
    q->tail = t;
}

static hermes_thread_t *queue_pop(hermes_wait_queue_t *q) {
    hermes_thread_t *t = q->head;
    if (!t) return NULL;
    q->head = t->queue_next;
    if (!q->head) q->tail = NULL;
    t->queue_next = NULL;
    t->queue = NULL;
    return t;
}

static void queue_unlink(hermes_thread_t *t) {
    hermes_wait_queue_t *q = t->queue;
    hermes_thread_t *prev = NULL;
    for (hermes_thread_t *it = q->head; it; prev = it, it = it->queue_next) {
        if (it != t) continue;
        if (prev) prev->queue_next = it->queue_next;
        else q->head = it->queue_next;
        if (q->tail == it) q->tail = prev;
        break;
    }
    t->queue_next = NULL;
    t->queue = NULL;
}

static void deadlock(const char *what) {
    hermes_thread_t *self = g_current;
    while (g_lock_depth > 0) hermes_unlock();
    HERMES_DIAG("hermes: deadlock detected (%" HERMES_PS "; thread '%s' on core %d)\n",
                what, self ? self->device.name : "?", g_core_index);
    (void)what; (void)self;
    hermes_fatal();
}

__attribute__((constructor))
static void hermes_scheduler_bootstrap(void) {
#if HERMES_BARE_METAL
    main_canary_arm(); /* first: before anything allocates */
#endif
    /* `main` is a thread too, so it occupies a registry slot like any
     * other -- the registry lists every live device, not just the ones a
     * .hepy program created explicitly. */
    hermes_device_register_or_abort(&g_main_thread.device, &hermes_device_class_thread, "main");
    g_main_thread.state = HERMES_THREAD_RUNNING;
    g_main_thread.id = 0;
    g_main_thread.core = 0;
    g_main_thread.on_cpu = 1;
    g_main_thread.is_root = 1;
    g_main_thread.alive = 1;
    g_current = &g_main_thread;
#if !HERMES_BARE_METAL
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    g_core_count = n < 1 ? 1 : n > HERMES_MAX_CORES ? HERMES_MAX_CORES : (int)n;
#endif
}

/* --- switching -----------------------------------------------------------
 * Both called with the lock held exactly once. */

/* Runs `t` here until it gives control back. Returns with the lock held. */
/* --- canaries (see hermes_thread.h) --------------------------------------- */

static const unsigned char HERMES_CANARY[8] = { 0xC4, 0x9A, 0x2E, 0x51, 0xB7, 0x0D, 0xE3, 0x6F };

static void canary_write(unsigned char *at) {
    for (int i = 0; i < HERMES_CANARY_BYTES; i++) at[i] = HERMES_CANARY[i];
}

static int canary_ok(const unsigned char *at) {
    for (int i = 0; i < HERMES_CANARY_BYTES; i++) if (at[i] != HERMES_CANARY[i]) return 0;
    return 1;
}

/* Roots (main, the cores' bases) have no block. Decided by address, not
 * by a field of the TCB: the TCB may be the very thing that got
 * overwritten. */
static int is_root_ptr(hermes_thread_t *t);

/* Lock held (any depth). The start canary is found from the TCB's own
 * address (it sits right below it), so it's checked before trusting
 * anything inside the TCB: an overflow from below reaches the canary
 * before the TCB. Only if it's intact are the TCB's fields (size, name)
 * used, for the end canary and the message. */
static void canary_check_locked(hermes_thread_t *t) {
    if (is_root_ptr(t)) return;
    if (!canary_ok((unsigned char *)t - HERMES_CANARY_BYTES)) {
        /* Its TCB may be garbage, including its link in the thread list:
         * cut the list there so hermes_fatal can still walk every other
         * kind (and the threads before it) to leave the hardware safe. */
        hermes_device_t *prev = NULL;
        for (hermes_device_t *d = hermes_device_class_thread.head; d; prev = d, d = d->next) {
            if (d != &t->device) continue;
            if (prev) prev->next = NULL;
            else hermes_device_class_thread.head = NULL;
            hermes_device_class_thread.tail = prev;
            break;
        }
        while (g_lock_depth > 0) hermes_unlock();
        HERMES_DIAG("hermes: memory corruption in thread %p: start canary overwritten "
                    "(its control block may be damaged)\n", (void *)t);
        hermes_fatal();
    }
    if (canary_ok(t->block_start + t->block_size - HERMES_CANARY_BYTES)) return;
    while (g_lock_depth > 0) hermes_unlock();
    HERMES_DIAG("hermes: memory corruption in thread '%s' (id %d): end canary overwritten\n",
                t->device.name, t->id);
    hermes_fatal();
}

/* A thread that finds its own start canary broken when giving control
 * back can't trust anything: its stack pointer may already be outside its
 * block, and its TCB (where its resumer is) may be garbage. So it doesn't
 * report on its own stack: it jumps to its core's root context (main, or
 * the core's base -- found by address, not through the TCB), which is
 * always waiting inside resume_locked, and the root reports it from a
 * sound stack. */
HERMES_TLS_VAR(hermes_thread_t *, g_corrupted, NULL)
#define g_corrupted (*g_corrupted_addr())
HERMES_TLS_VAR(hermes_ctx_t, g_corrupted_ctx, {0}) /* where its registers go */
#define g_corrupted_ctx (*g_corrupted_ctx_addr())

static hermes_thread_t *core_root(void) {
#if !HERMES_BARE_METAL
    if (g_core_index != 0) return &g_cores[g_core_index].base;
#endif
    return &g_main_thread;
}

/* Lock held exactly once; released for the switch. Bare metal: the lock
 * is released but interrupts stay masked through the whole switch -- the
 * tick may switch threads from its interrupt (preemption), so it must
 * never find one half done -- and each context, when it's resumed, gets
 * back the interrupt state it had when it took the lock: enabled for
 * ordinary code, disabled for a thread preempted inside the tick's
 * interrupt (its reti/mret enables them). */
static void switch_unlocked(hermes_ctx_t *from, hermes_ctx_t *to) {
#if HERMES_BARE_METAL
    hermes_irq_state_t s = g_lock_irq;
    g_lock_depth = 0;
    hermes_ctx_switch(from, to);
    irq_restore(s);
#else
    hermes_unlock();
    hermes_ctx_switch(from, to);
#endif
}

static void resume_locked(hermes_thread_t *t) {
    hermes_thread_t *self = g_current;
    t->resumer = self;
    t->state = HERMES_THREAD_RUNNING;
    t->on_cpu = 1;
    g_current = t;
    switch_unlocked(&self->ctx, &t->ctx);
    hermes_lock();
    if (g_corrupted) canary_check_locked(g_corrupted); /* fatal */
    t->on_cpu = 0; /* its context is saved: back to us for good or for now */
    canary_check_locked(t);
    notify_locked();
}

/* The current (non-root) thread gives control back to its resumer, leaving
 * itself in `st` (unless it was aborted meanwhile). Returns, with the lock
 * RELEASED, if it is ever resumed. */
static void switch_out_locked(hermes_thread_state_t st) {
    hermes_thread_t *self = g_current;
    if (!canary_ok((unsigned char *)self - HERMES_CANARY_BYTES)) {
        /* before using anything in its TCB (resumer...): see g_corrupted */
        hermes_thread_t *root = core_root();
        g_corrupted = self;
        g_current = root;
        switch_unlocked(&g_corrupted_ctx, &root->ctx); /* never comes back */
    }
    hermes_thread_t *back = self->resumer;
    if (self->state != HERMES_THREAD_TERMINATED) self->state = st;
    self->resumer = NULL;
    g_current = back;
    switch_unlocked(&self->ctx, &back->ctx);
}

/* Resumes `t` (bound to this core) every time it's runnable, waits while
 * it's blocked or paused, until it finishes. */
static void drive_locked(hermes_thread_t *t) {
    while (t->state != HERMES_THREAD_TERMINATED) {
        if (t->state == HERMES_THREAD_READY && !t->paused) resume_locked(t);
        else idle_wait();
    }
}

static void terminated_locked(hermes_thread_t *t);

static void hermes_thread_trampoline(void) {
    hermes_thread_t *self = g_current;
#if HERMES_BARE_METAL
    irq_restore(g_lock_irq); /* first run: arrived as switch_unlocked leaves it, masked */
#endif
    self->entry(self->arg);
    hermes_lock();
    self->state = HERMES_THREAD_TERMINATED;
    terminated_locked(self);
    notify_locked();
    switch_out_locked(HERMES_THREAD_TERMINATED);

    HERMES_DIAG("hermes: thread '%s' resumed after it had finished\n", self->device.name);
    hermes_fatal();
}

#if !HERMES_BARE_METAL
static void *core_main(void *arg) {
    hermes_core_t *c = arg;
    int k = (int)(c - g_cores);
    g_core_index = k;
    g_current = &c->base;
    hermes_lock();
    for (;;) {
        if (hermes_os_hooks && hermes_os_hooks->running) {
            hermes_os_hooks->core_loop(); /* until os_run() ends */
            continue;
        }
        hermes_thread_t *t = queue_pop(&c->queue);
        if (!t) t = queue_pop(&g_launch_any);
        if (!t) { idle_wait_on(&g_work_cond); continue; }
        t->core = (signed char)k;
        c->busy = 1;
        drive_locked(t);
        c->busy = 0;
        notify_locked();
    }
    return NULL;
}

/* Starts every other core the first time a thread is launched (so a
 * program that never launches anything never creates a pthread). */
static void start_cores_locked(void) {
    if (g_workers_started) return;
    g_workers_started = 1;
    for (int k = 1; k < g_core_count; k++) {
        hermes_core_t *c = &g_cores[k];
        c->base.device.name = "core";
        c->base.state = HERMES_THREAD_RUNNING;
        c->base.id = -k;
        c->base.core = (signed char)k;
        c->base.on_cpu = 1;
        c->base.is_root = 1;
        c->base.alive = 1;
        pthread_t tid;
        g_active++;
        if (pthread_create(&tid, NULL, core_main, c) != 0) {
            g_active--;
            HERMES_DIAG("hermes: could not start core %d; using %d\n", k, k);
            g_core_count = k;
            break;
        }
        pthread_detach(tid);
    }
}
#endif

/* --- what the OS library (hermes_os) builds on --------------------------- */

hermes_os_hooks_t *hermes_os_hooks = NULL;

/* --- giving the core back on request (see hermes_sched_request_switch) ---
 * One flag per core: the OS tick sets it. On bare metal the tick itself
 * then takes the core from the thread, right in its interrupt
 * (hermes_sched_preempt: preemption); on the host the thread sees the flag
 * at its next safe point -- hermes_lock taken from outside the framework,
 * which is where nothing of the framework is half done -- and yields. */
static volatile unsigned char g_switch_due[HERMES_MAX_CORES];

void hermes_sched_request_switch(int core) { g_switch_due[core] = 1; }

/* Whether the current thread has to give its core back now. Only a
 * process thread does: never main or a core's base, never inside
 * os_preempt_off(), never once a fatal error has started. Clears the
 * request when it says yes. */
static int switch_due_now(void) {
    if (!hermes_os_hooks || !hermes_os_hooks->running || hermes_fatal_started) return 0;
    int core = g_core_index;
    if (!g_switch_due[core]) return 0;
    hermes_thread_t *self = g_current;
    if (!self || is_root_ptr(self) || !self->in_process || self->no_preempt) return 0;
    g_switch_due[core] = 0;
    return 1;
}

/* Called by hermes_lock when the lock isn't held (by this core). Never
 * from an interrupt (bare metal: interrupts are masked inside one -- the
 * tick itself takes the lock): that is hermes_sched_preempt's job. */
static void switch_point(void) {
#if HERMES_BARE_METAL
    if (!irq_enabled()) return;
#endif
    if (switch_due_now()) hermes_thread_yield(); /* takes the lock again: the flag is clear now */
}

void hermes_sched_preempt(void) {
#if HERMES_BARE_METAL
    /* the interrupt came with interrupts enabled, so the interrupted code
     * didn't hold the lock and wasn't switching (switch_unlocked masks
     * them): any thread can be left here and resumed later */
    if (g_lock_depth == 0 && switch_due_now()) hermes_thread_yield();
#endif
}

void hermes_preempt_off(void) {
    g_current->no_preempt++; /* only its own thread writes it */
}

void hermes_preempt_on(void) {
    hermes_thread_t *self = g_current;
    if (self->no_preempt && --self->no_preempt == 0 && g_lock_depth == 0) {
        hermes_lock(); /* a switch put off meanwhile: the safe point does it */
        hermes_unlock();
    }
}

/* A request is for the thread that was running: a core that resumes one
 * starts with a clean slate. */
void hermes_sched_resume(hermes_thread_t *t) {
    g_switch_due[g_core_index] = 0;
    resume_locked(t);
}
void hermes_sched_idle(void) { idle_wait(); }
void hermes_sched_notify(void) { notify_work_locked(); }
void hermes_cores_start(void) {
#if !HERMES_BARE_METAL
    start_cores_locked();
#endif
}
void hermes_queue_push(hermes_wait_queue_t *q, hermes_thread_t *t) { queue_push(q, t); }
void hermes_queue_unlink(hermes_thread_t *t) { if (t->queue) queue_unlink(t); }
static void check_live_locked(hermes_thread_t *t, const char *op);
void hermes_thread_check_live(hermes_thread_t *t, const char *op) { check_live_locked(t, op); }

/* Under hermes_os the scheduler decides who runs: the manual operations
 * are refused on the process's threads. */
static void refuse_in_process_locked(hermes_thread_t *t, const char *op) {
    if (!t || !t->in_process) return;
    while (g_lock_depth > 0) hermes_unlock();
    HERMES_DIAG("hermes: %" HERMES_PS "() on a process thread: with hermes_os the scheduler decides "
                "who runs (use os_run())\n", op);
    (void)op;
    hermes_fatal();
}

/* Lock held. A thread just terminated (its function returned, or abort):
 * whoever was blocked waiting for it goes on, and the OS is told. */
static int wake_one_locked(hermes_wait_queue_t *q);
static void terminated_locked(hermes_thread_t *t) {
    while (wake_one_locked(&t->joiners)) {}
    if (t->in_process && hermes_os_hooks) hermes_os_hooks->terminated(t);
}

/* --- public API -----------------------------------------------------------
 * Every operation on a thread first checks its `alive` flag (set by
 * create, cleared by destroy): a destroyed thread (or NULL) is a fatal
 * error. Lock held. */

static int is_live_locked(hermes_thread_t *t) {
    return t && t->alive;
}

static void check_live_locked(hermes_thread_t *t, const char *op) {
    if (is_live_locked(t)) return;
    while (g_lock_depth > 0) hermes_unlock();
    HERMES_DIAG("hermes: %" HERMES_PS "() on a destroyed or nonexistent thread\n", op);
    (void)op;
    hermes_fatal();
}

static void check_live(hermes_thread_t *t, const char *op) {
    hermes_lock();
    check_live_locked(t, op);
    hermes_unlock();
}

hermes_thread_t *hermes_thread_create_ex(hermes_thread_fn entry, long arg,
                                          const hermes_thread_attr_t *attr) {
    size_t heap_size = (attr && attr->heap_size) ? attr->heap_size : HERMES_THREAD_DEFAULT_HEAP_SIZE;
    if (heap_size == HERMES_THREAD_NO_HEAP) heap_size = 0;
    size_t stack_size = (attr && attr->stack_size) ? attr->stack_size : HERMES_THREAD_DEFAULT_STACK_SIZE;
    const char *name = (attr && attr->name) ? attr->name : "thread";

    size_t total = HERMES_CANARY_BYTES + sizeof(hermes_thread_t) + heap_size + stack_size
                 + HERMES_CANARY_BYTES;
    /* under hermes_os the block lives in the process's regions */
    unsigned char *block = hermes_os_hooks ? hermes_os_hooks->block_alloc(total) : hermes_runtime_alloc(total);
    if (!block) {
        HERMES_DIAG("hermes_thread_create: out of memory (%zu bytes)\n", total);
        hermes_fatal();
    }

    canary_write(block);
    canary_write(block + total - HERMES_CANARY_BYTES);
    hermes_thread_t *t = (hermes_thread_t *)(block + HERMES_CANARY_BYTES);
    t->state = HERMES_THREAD_READY;
    t->entry = entry;
    t->arg = arg;
    t->block_start = block;
    t->block_size = total;
    t->heap_cursor = (unsigned char *)t + sizeof(hermes_thread_t);
    t->heap_limit = t->heap_cursor + heap_size;
    t->resumer = NULL;
    t->queue_next = NULL;
    t->queue = NULL;
    t->core = -1;
    t->paused = 0;
    t->on_cpu = 0;
    t->is_root = 0;
    t->alive = 1;
    t->joiners.head = NULL;
    t->joiners.tail = NULL;
    t->in_process = hermes_os_hooks ? 1 : 0;
    t->priority = 4;
    t->pin = -1;
    t->next_core = -1;
    t->no_preempt = 0;

    /* highest address of the stack: right below the end canary */
    unsigned char *stack_base = block + total - HERMES_CANARY_BYTES;
    hermes_ctx_make(&t->ctx, stack_base, stack_size, hermes_thread_trampoline);

    hermes_lock();
    t->id = g_next_id++;
    hermes_unlock();
    hermes_device_register_or_abort(&t->device, &hermes_device_class_thread, name);
    if (t->in_process) {
        hermes_lock();
        hermes_os_hooks->created(t); /* into the process and the scheduler */
        hermes_unlock();
    }
    return t;
}

hermes_thread_t *hermes_thread_create(hermes_thread_fn entry, long arg) {
    return hermes_thread_create_ex(entry, arg, NULL);
}

void *hermes_thread_alloc(hermes_thread_t *t, size_t size) {
    check_live(t, HERMES_PSTR("alloc"));
    size_t aligned = (size + (sizeof(void *) - 1)) & ~(sizeof(void *) - 1);
    if (t->heap_cursor + aligned > t->heap_limit) {
        HERMES_DIAG("hermes_thread_alloc: thread '%s' out of heap memory (%zu bytes requested)\n",
                t->device.name, size);
        return NULL;
    }
    void *p = t->heap_cursor;
    t->heap_cursor += aligned;
    return p;
}

void hermes_thread_run(hermes_thread_t *t) {
    hermes_lock();
    check_live_locked(t, HERMES_PSTR("run"));
    refuse_in_process_locked(t, HERMES_PSTR("run"));
    if (t != g_current && !t->is_root && t->state == HERMES_THREAD_READY && !t->paused &&
        (t->core < 0 || t->core == g_core_index)) {
        if (t->queue) queue_unlink(t); /* a READY thread can only be in a launch queue */
        t->core = (signed char)g_core_index;
        resume_locked(t);
    }
    hermes_unlock();
}

void hermes_thread_launch(hermes_thread_t *t) {
    hermes_lock();
    check_live_locked(t, HERMES_PSTR("launch"));
    refuse_in_process_locked(t, HERMES_PSTR("launch"));
    if (!t->is_root && t->state == HERMES_THREAD_READY && t->core < 0 && !t->queue) {
        queue_push(&g_launch_any, t);
#if !HERMES_BARE_METAL
        start_cores_locked();
#endif
        notify_work_locked();
    }
    hermes_unlock();
}

void hermes_thread_launch_on(hermes_thread_t *t, int core) {
    hermes_lock();
    check_live_locked(t, HERMES_PSTR("launch_on"));
    refuse_in_process_locked(t, HERMES_PSTR("launch_on"));
    if (core < 1 || core >= hermes_cores_count()) {
        hermes_unlock();
        HERMES_DIAG("hermes: launch_on(%d): no such core (there are %d; core 0 is main's: "
                    "use run())\n", core, hermes_cores_count());
        hermes_fatal();
    }
#if !HERMES_BARE_METAL
    if (!t->is_root && t->state == HERMES_THREAD_READY && t->core < 0 && !t->queue) {
        queue_push(&g_cores[core].queue, t);
        start_cores_locked();
        notify_work_locked();
    }
#else
    (void)t;
#endif
    hermes_unlock();
}

void hermes_thread_wait(hermes_thread_t *t) {
    hermes_lock();
    check_live_locked(t, HERMES_PSTR("wait"));
    if (t != g_current && !t->is_root) {
        if (g_current->in_process && t->state != HERMES_THREAD_TERMINATED) {
            /* a scheduled thread doesn't hold its core while waiting: it
             * blocks, and the scheduler runs others meanwhile */
            hermes_sched_block_current(&t->joiners); /* returns unlocked */
            return;
        }
        /* another core may destroy it meanwhile: then it has finished */
        while (is_live_locked(t) && (t->state != HERMES_THREAD_TERMINATED || t->on_cpu)) idle_wait();
    }
    hermes_unlock();
}

int hermes_thread_done(hermes_thread_t *t) {
    hermes_lock();
    check_live_locked(t, HERMES_PSTR("done"));
    int done = t->state == HERMES_THREAD_TERMINATED;
    hermes_unlock();
    return done;
}

void hermes_thread_pause(hermes_thread_t *t) {
    hermes_lock();
    check_live_locked(t, HERMES_PSTR("pause"));
    if (!t->is_root) t->paused = 1;
    if (t->in_process && t->state == HERMES_THREAD_READY && t->queue) queue_unlink(t); /* off the ready set */
    hermes_unlock();
}

void hermes_thread_resume(hermes_thread_t *t) {
    hermes_lock();
    check_live_locked(t, HERMES_PSTR("resume"));
    if (t->paused && t->in_process && t->state == HERMES_THREAD_READY && !t->queue && !t->on_cpu) {
        t->paused = 0;
        hermes_os_hooks->ready(t); /* back to the scheduler */
    }
    t->paused = 0;
    notify_locked();
    hermes_unlock();
}

void hermes_thread_yield(void) {
    hermes_lock();
    if (is_root_ptr(g_current)) { hermes_unlock(); return; }
    switch_out_locked(HERMES_THREAD_READY);
}

hermes_thread_t *hermes_thread_current(void) {
    return g_current;
}

/* Lock held (any depth). Only on the current thread does it switch away,
 * and then the lock must be held exactly once. */
static void abort_locked(hermes_thread_t *t) {
    if (t->is_root || t->state == HERMES_THREAD_TERMINATED) return;
    t->state = HERMES_THREAD_TERMINATED;
    if (t->queue) queue_unlink(t);
    terminated_locked(t);
    notify_locked();
    if (t != g_current) return;

    /* aborting itself: leave exactly like a thread whose function returned */
    switch_out_locked(HERMES_THREAD_TERMINATED);
    HERMES_DIAG("hermes: thread '%s' resumed after aborting itself\n", t->device.name);
    hermes_fatal();
}

void hermes_thread_abort(hermes_thread_t *t) {
    hermes_lock();
    check_live_locked(t, HERMES_PSTR("abort"));
    abort_locked(t);
    hermes_unlock();
}

void hermes_thread_destroy(hermes_thread_t *t) {
    hermes_lock();
    check_live_locked(t, HERMES_PSTR("destroy"));
    if (t->is_root) { hermes_unlock(); return; }
    abort_locked(t); /* on itself it never comes back */
    /* live on this very core = it's in the chain of run() calls that led
     * here: its stack is still in use below us */
    if (t->on_cpu && t->core == g_core_index) { hermes_unlock(); return; }
    while (t->on_cpu) idle_wait();
    canary_check_locked(t); /* last chance to notice before it's released */
    t->alive = 0; /* from here on, t is "destroyed" */
    hermes_device_unregister(&t->device);
    hermes_unlock();
    unsigned char *block = t->block_start;
    int in_process = t->in_process;
    memset(t, 0, sizeof(*t)); /* its data reads as 0/NULL until reused */
    if (in_process) hermes_os_hooks->block_free(block);
    else hermes_runtime_free(block);
}

int hermes_thread_id(hermes_thread_t *t) {
    check_live(t, HERMES_PSTR("thread_id"));
    return t->id;
}

int hermes_cores_count(void) {
#if HERMES_BARE_METAL
    return 1;
#else
    return g_core_count;
#endif
}

int hermes_core_current(void) {
    return g_core_index;
}

int hermes_cores_free(void) {
    int n = 1; /* the caller's own core */
#if !HERMES_BARE_METAL
    hermes_lock();
    for (int k = 1; k < g_core_count; k++) {
        if (k == g_core_index) continue;
        if (!g_cores[k].busy && !g_cores[k].queue.head) n++;
    }
    hermes_unlock();
#endif
    return n;
}

/* The class's abort (Total_abort and the fatal path): stops every thread
 * except the one running the abort -- it must be able to go on, e.g. to
 * report it -- and main. Called with the lock held. */
static void thread_abort_entry(hermes_device_t *d) {
    hermes_thread_t *t = (hermes_thread_t *)d;
    if (t != g_current) abort_locked(t);
}

/* --- runtime hooks (see hermes_sched_internal.h) -------------------------- */

void hermes_sched_block_current(hermes_wait_queue_t *q) {
    hermes_thread_t *self = g_current;
    if (self->state == HERMES_THREAD_TERMINATED) {
        /* aborted from another core while running: stop here */
        switch_out_locked(HERMES_THREAD_TERMINATED);
    }
    self->state = HERMES_THREAD_BLOCKED;
    queue_push(q, self);
    if (self->is_root) {
        /* nobody to give control to: wait right here */
        while (self->state == HERMES_THREAD_BLOCKED) idle_wait();
        self->state = HERMES_THREAD_RUNNING;
        hermes_unlock();
        return;
    }
    switch_out_locked(HERMES_THREAD_BLOCKED);
}

static int wake_one_locked(hermes_wait_queue_t *q) {
    hermes_thread_t *t = queue_pop(q);
    if (!t) return 0;
    t->state = HERMES_THREAD_READY;
    if (t->in_process && hermes_os_hooks && !t->paused) hermes_os_hooks->ready(t);
    notify_locked();
    return 1;
}

int hermes_sched_wake_one(hermes_wait_queue_t *q) {
    return wake_one_locked(q);
}

int hermes_thread_take(hermes_thread_t *t) {
    hermes_lock();
    int taken = 0;
    if (t->queue && t->state == HERMES_THREAD_READY && t->core < 0) {
        queue_unlink(t);
        t->core = (signed char)g_core_index;
        taken = 1;
    }
    hermes_unlock();
    return taken;
}

void hermes_thread_run_to_end(hermes_thread_t *t) {
    hermes_lock();
    drive_locked(t);
    hermes_unlock();
}
