#include "hermes_thread.h"
#include "hermes_sched_internal.h"
#include "hermes_ctx.h"
#include <stdlib.h>

/* Diagnostics are a plain no-op on AVR: there is no stream wired to
 * anything yet (src/framework/drivers doesn't exist), and `fprintf`'s
 * underlying formatting engine costs ~1.5 KB of flash on its own (measured
 * with avr-size) -- a real cost on a 32 KB part, paid by every program that
 * uses hermes_threads even if it never calls print(). On the host there's
 * no such constraint, so keep the normal fprintf there. */
#if defined(__AVR__)
#define HERMES_DIAG(...) ((void)0)
#else
#include <stdio.h>
#define HERMES_DIAG(...) fprintf(stderr, __VA_ARGS__)
#endif

/* --- hermes_runtime_alloc/free: single allocation point for this module
 * and hermes_sem.c (declared in hermes_sched_internal.h). Two bodies,
 * selected by whether a memory-profile import ('light'/'medium'/'heavy',
 * see src/engine/compiler/mempool.c) defined HERMES_POOL_BYTES for this
 * build:
 *   - undefined: plain malloc/free, unbounded, dynamic (today's behavior).
 *   - defined: a bump allocator over one static array of that many bytes
 *     -- true static allocation, no heap/fragmentation at all, at the cost
 *     of a hard ceiling on total thread+semaphore memory fixed at compile
 *     time. Never reclaims (see the comment on the declaration for why
 *     that's not a regression). Available on any target, not just AVR --
 *     importing a profile together with 'native' lets the same ceiling be
 *     exercised and tested on the host before flashing real hardware. */
#ifdef HERMES_POOL_BYTES

static unsigned char hermes_pool[HERMES_POOL_BYTES];
static size_t hermes_pool_used = 0;

void *hermes_runtime_alloc(size_t n) {
    if (hermes_pool_used + n > HERMES_POOL_BYTES) return NULL;
    void *p = hermes_pool + hermes_pool_used;
    hermes_pool_used += n;
    return p;
}

void hermes_runtime_free(void *p) {
    (void)p; /* bump allocator: nothing to reclaim */
}

#else

#if defined(__AVR__)
#include <avr/io.h>
/* Bytes at the top of RAM kept for main()'s own stack when allocating
 * with malloc (no memory profile). See hermes_runtime_alloc below. */
#ifndef HERMES_AVR_MAIN_STACK_BYTES
#define HERMES_AVR_MAIN_STACK_BYTES 512
#endif
extern char *__malloc_heap_end;
#endif

void *hermes_runtime_alloc(size_t n) {
#if defined(__AVR__)
    /* avr-libc's malloc, when __malloc_heap_end is 0 (the default), takes
     * "current SP - __malloc_margin" as the heap's upper limit. From inside
     * a hermes thread the SP lives in that thread's own block -- i.e. in
     * the heap itself -- so malloc concludes there's no room and returns
     * NULL: every device/thread created from a thread aborted (found when
     * the UART driver first made AVR output visible; devices_demo's
     * gpio() inside a thread had been aborting silently). A fixed limit
     * makes it independent of which stack is current. */
    if (!__malloc_heap_end) {
        __malloc_heap_end = (char *)(RAMEND + 1 - HERMES_AVR_MAIN_STACK_BYTES);
    }
#endif
    return malloc(n);
}

void hermes_runtime_free(void *p) {
    free(p);
}

#endif

/* The thread's device class (see hermes_device.h): the one and only
 * definition, so its address is the single identity of "a thread"; it
 * also holds the list of every thread. No hardware limit. */
static void thread_abort_entry(hermes_device_t *d);
hermes_device_class_t hermes_device_class_thread = {
    .name = "thread", .max_instances = 0, .abort = thread_abort_entry
};

static hermes_thread_t g_main_thread;
static hermes_thread_t *g_current = NULL;

static hermes_thread_t *g_ready_head = NULL;
static hermes_thread_t *g_ready_tail = NULL;

static int g_next_id = 1;

static void ready_enqueue(hermes_thread_t *t) {
    t->queue_next = NULL;
    if (g_ready_tail) g_ready_tail->queue_next = t;
    else g_ready_head = t;
    g_ready_tail = t;
}

/* Aborted threads (hermes_thread_abort) are left wherever they were
 * queued and simply dropped when they reach the front -- no need to find
 * and unlink them from the ready queue or a semaphore's wait queue. */
static hermes_thread_t *ready_dequeue(void) {
    hermes_thread_t *t;
    do {
        t = g_ready_head;
        if (!t) return NULL;
        g_ready_head = t->queue_next;
        if (!g_ready_head) g_ready_tail = NULL;
        t->queue_next = NULL;
    } while (t->state == HERMES_THREAD_TERMINATED);
    return t;
}

__attribute__((constructor))
static void hermes_scheduler_bootstrap(void) {
    /* `main` is a thread too, so it occupies a registry slot like any
     * other -- the registry lists every live device, not just the ones a
     * .hepy program created explicitly. */
    hermes_device_register_or_abort(&g_main_thread.device, &hermes_device_class_thread, "main");
    g_main_thread.state = HERMES_THREAD_RUNNING;
    g_main_thread.entry = NULL;
    g_main_thread.arg = 0;
    g_main_thread.id = 0;
    g_main_thread.block_start = NULL;
    g_main_thread.block_size = 0;
    g_main_thread.heap_cursor = NULL;
    g_main_thread.heap_limit = NULL;
    g_main_thread.queue_next = NULL;
    g_current = &g_main_thread;
}

static void switch_to(hermes_thread_t *next) {
    hermes_thread_t *prev = g_current;
    g_current = next;
    next->state = HERMES_THREAD_RUNNING;
    hermes_ctx_switch(&prev->ctx, &next->ctx);
}

static void hermes_thread_trampoline(void) {
    hermes_thread_t *self = g_current;
    self->entry(self->arg);
    self->state = HERMES_THREAD_TERMINATED;

    hermes_thread_t *next = ready_dequeue();
    if (!next) next = &g_main_thread;
    switch_to(next);

    HERMES_DIAG("hermes: hilo '%s' resumido tras haber terminado\n", self->device.name);
    hermes_fatal();
}

hermes_thread_t *hermes_thread_create_ex(hermes_thread_fn entry, long arg,
                                          const hermes_thread_attr_t *attr) {
    size_t heap_size = (attr && attr->heap_size) ? attr->heap_size : HERMES_THREAD_DEFAULT_HEAP_SIZE;
    size_t stack_size = (attr && attr->stack_size) ? attr->stack_size : HERMES_THREAD_DEFAULT_STACK_SIZE;
    const char *name = (attr && attr->name) ? attr->name : "thread";

    size_t total = sizeof(hermes_thread_t) + heap_size + stack_size;
    unsigned char *block = hermes_runtime_alloc(total);
    if (!block) {
#ifdef HERMES_POOL_BYTES
        HERMES_DIAG("hermes_thread_create: pool estatico agotado (pedidos %zu bytes, pool de %d bytes)\n",
                    total, (int)HERMES_POOL_BYTES);
#else
        HERMES_DIAG("hermes_thread_create: sin memoria (%zu bytes)\n", total);
#endif
        hermes_fatal();
    }

    hermes_thread_t *t = (hermes_thread_t *)block;
    hermes_device_register_or_abort(&t->device, &hermes_device_class_thread, name);
    t->state = HERMES_THREAD_READY;
    t->entry = entry;
    t->arg = arg;
    t->id = g_next_id++;
    t->block_start = block;
    t->block_size = total;
    t->heap_cursor = block + sizeof(hermes_thread_t);
    t->heap_limit = t->heap_cursor + heap_size;
    t->queue_next = NULL;

    unsigned char *stack_base = block + total; /* highest address of the block */
    hermes_ctx_make(&t->ctx, stack_base, stack_size, hermes_thread_trampoline);

    ready_enqueue(t);
    return t;
}

hermes_thread_t *hermes_thread_create(hermes_thread_fn entry, long arg) {
    return hermes_thread_create_ex(entry, arg, NULL);
}

void *hermes_thread_alloc(hermes_thread_t *t, size_t size) {
    size_t aligned = (size + (sizeof(void *) - 1)) & ~(sizeof(void *) - 1);
    if (t->heap_cursor + aligned > t->heap_limit) {
        HERMES_DIAG("hermes_thread_alloc: hilo '%s' sin memoria en su heap (pedido %zu bytes)\n",
                t->device.name, size);
        return NULL;
    }
    void *p = t->heap_cursor;
    t->heap_cursor += aligned;
    return p;
}

void hermes_thread_yield(void) {
    hermes_thread_t *prev = g_current;
    hermes_thread_t *next = ready_dequeue();
    if (!next) return; /* nobody else ready: keep running prev */
    if (prev->state == HERMES_THREAD_RUNNING) {
        prev->state = HERMES_THREAD_READY;
        ready_enqueue(prev);
    }
    switch_to(next);
}

void hermes_thread_join(hermes_thread_t *t) {
    while (t->state != HERMES_THREAD_TERMINATED) {
        hermes_thread_yield();
    }
}

hermes_thread_t *hermes_thread_current(void) {
    return g_current;
}

void hermes_threads_run(void) {
    /* every thread is in the thread kind's device list (main first, then
     * creation order; threads created meanwhile are appended, so they're
     * reached too). The device is the thread's first member. */
    for (hermes_device_t *d = hermes_device_class_thread.head; d; d = d->next) {
        hermes_thread_t *t = (hermes_thread_t *)d;
        if (t != &g_main_thread && t != g_current) hermes_thread_join(t);
    }
}

void hermes_thread_abort(hermes_thread_t *t) {
    if (t == &g_main_thread || t->state == HERMES_THREAD_TERMINATED) return;
    t->state = HERMES_THREAD_TERMINATED;
    if (t != g_current) return; /* dropped from its queue when reached */

    /* aborting itself: leave exactly like a thread whose function returned */
    hermes_thread_t *next = ready_dequeue();
    if (!next) next = &g_main_thread;
    switch_to(next);
    HERMES_DIAG("hermes: hilo '%s' resumido tras abortarse\n", t->device.name);
    hermes_fatal();
}

/* The class's abort (Total_abort and the fatal path): stops every thread
 * except the one running the abort -- it must be able to go on, e.g. to
 * report it -- and main. */
static void thread_abort_entry(hermes_device_t *d) {
    hermes_thread_t *t = (hermes_thread_t *)d;
    if (t != g_current) hermes_thread_abort(t);
}

/* --- scheduler hooks used by hermes_sem.c (see hermes_sched_internal.h) */

void hermes_sched_block_current(hermes_wait_queue_t *q) {
    hermes_thread_t *prev = g_current;
    prev->state = HERMES_THREAD_BLOCKED;
    prev->queue_next = NULL;
    if (q->tail) q->tail->queue_next = prev;
    else q->head = prev;
    q->tail = prev;

    hermes_thread_t *next = ready_dequeue();
    if (!next) {
        HERMES_DIAG("hermes: deadlock detectado (el hilo '%s' se bloquea y no hay ningun otro listo)\n",
                prev->device.name);
        hermes_fatal();
    }
    switch_to(next);
}

int hermes_sched_wake_one(hermes_wait_queue_t *q) {
    hermes_thread_t *t;
    do { /* skip threads aborted while waiting: they must not come back */
        t = q->head;
        if (!t) return 0;
        q->head = t->queue_next;
        if (!q->head) q->tail = NULL;
        t->queue_next = NULL;
    } while (t->state == HERMES_THREAD_TERMINATED);
    t->state = HERMES_THREAD_READY;
    ready_enqueue(t);
    return 1;
}
