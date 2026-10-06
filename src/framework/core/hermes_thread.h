#ifndef HERMES_THREAD_H
#define HERMES_THREAD_H

#include <stddef.h>
#include "hermes_ctx.h"
#include "hermes_device.h"

typedef enum {
    HERMES_THREAD_READY,
    HERMES_THREAD_RUNNING,
    HERMES_THREAD_BLOCKED,
    HERMES_THREAD_TERMINATED
} hermes_thread_state_t;

/* Thread entry points take a single `long` argument. HERMESPY v0.1 has no
 * address-of operator and no raw pointer types, so a `long` (rather than
 * `void *`) is what a .hepy program can actually construct and pass; see
 * docs/raw/hermes_threads.en.md. */
typedef void (*hermes_thread_fn)(long arg);

/* --- per-thread memory block --------------------------------------------
 *
 *   +-----------------------------+  <- block_start (lowest address)
 *   | hermes_thread_t (the TCB)   |  key thread data; the TCB IS the start
 *   +-----------------------------+  of its own block, not a separate alloc
 *   | heap area (bump allocator)  |  grows upward -- hermes_thread_alloc()
 *   +-----------------------------+
 *   | stack area                  |  grows downward, used by the CPU/ctx
 *   +-----------------------------+  <- block_start + block_size (highest)
 */
typedef struct hermes_thread {
    hermes_device_t device; /* must be first -- makes a hermes_thread_t* a
                                valid hermes_device_t* (see hermes_device.h);
                                device.name replaces what used to be a
                                separate `name` field here */
    hermes_ctx_t ctx;
    hermes_thread_state_t state;
    hermes_thread_fn entry;
    long arg;
    int id;

    unsigned char *block_start;
    size_t block_size;
    unsigned char *heap_cursor; /* next free byte in the heap area */
    unsigned char *heap_limit;  /* first byte NOT available to the heap
                                    (== first byte of the stack area) */

    /* Linkage reused for whichever single queue this thread currently sits
     * in: the scheduler's ready queue, or one semaphore's wait queue. A
     * thread is never in both at once, so one field suffices. */
    struct hermes_thread *queue_next;

    struct hermes_thread *all_next; /* linkage in the list of all created threads */
} hermes_thread_t;

typedef struct {
    size_t heap_size;  /* 0 => HERMES_THREAD_DEFAULT_HEAP_SIZE */
    size_t stack_size; /* 0 => HERMES_THREAD_DEFAULT_STACK_SIZE */
    const char *name;  /* NULL => "thread" */
} hermes_thread_attr_t;

#define HERMES_THREAD_DEFAULT_HEAP_SIZE  (4ul  * 1024ul)
#define HERMES_THREAD_DEFAULT_STACK_SIZE (16ul * 1024ul)

/* Automatic tier: sensible defaults, scheduling handled entirely by the
 * runtime from here on. */
hermes_thread_t *hermes_thread_create(hermes_thread_fn entry, long arg);

/* Fine-grained tier: explicit heap/stack split and name. */
hermes_thread_t *hermes_thread_create_ex(hermes_thread_fn entry, long arg,
                                          const hermes_thread_attr_t *attr);

/* Bump-allocates `size` bytes from thread `t`'s own heap area. Returns NULL
 * if `t`'s heap area is exhausted. */
void *hermes_thread_alloc(hermes_thread_t *t, size_t size);

void hermes_thread_yield(void);
void hermes_thread_join(hermes_thread_t *t);
hermes_thread_t *hermes_thread_current(void);

/* Automatic-tier convenience: blocks until every thread created so far has
 * terminated. The main context must remain inside this call (or inside
 * hermes_thread_join) for as long as any created thread still needs to run
 * -- the scheduler resumes `main` whenever no other thread is ready. */
void hermes_threads_run(void);

#endif
