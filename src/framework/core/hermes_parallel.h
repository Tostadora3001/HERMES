#ifndef HERMES_PARALLEL_H
#define HERMES_PARALLEL_H

/* Real multi-core parallelism via physical OS threads (pthreads on the
 * host). This is a SEPARATE concurrency model from hermes_threads.h's
 * cooperative scheduler, and the two must not be mixed: a `parallel_for`
 * body must never call anything from hermes_thread.h/hermes_sem.h
 * (thread_yield, sem_wait, ...) -- the cooperative scheduler's global
 * state (ready queue, g_current) is not safe to touch from more than one
 * real OS thread at a time.
 *
 * On AVR there is only one physical core, so `parallel_for` here just runs
 * the loop sequentially in the calling thread. Same .hepy source, same
 * result, just no speedup -- a program that does `import native` +
 * `import avr` compiles correctly for both with no extra plumbing in
 * hepyc's architecture registry (see src/engine/compiler/arch.c): the
 * branch below is a plain compile-time `#if`.
 *
 * Implementation note: `hermes_parallel_active_threads` has internal
 * linkage (effectively one copy per translation unit that includes this
 * header). That's fine as long as hepyc keeps generating a single .c file
 * per program, which is the case today -- if that ever changes, this
 * counter would need to move to a real .c file with external linkage to
 * stay accurate across translation units.
 */

typedef void (*hermes_parallel_body_fn)(long index);

#if defined(__AVR__)

static inline long parallel_cores_available(void) { return 1; }

static inline void parallel_for(long start, long end, hermes_parallel_body_fn body, long num_threads) {
    (void)num_threads;
    for (long i = start; i < end; i++) body(i);
}

#else /* host: real pthreads */

#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

static atomic_long hermes_parallel_active_threads = 0;

static inline long hermes_parallel_hw_concurrency(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

/* Hardware concurrency minus however many physical threads *this program*
 * currently has in flight via parallel_for (e.g. an outer parallel_for
 * calling into another one). Not a system-wide "idle core" oracle -- no
 * mainstream parallel-for implementation (OpenMP, TBB, ...) attempts that
 * either, since the OS scheduler doesn't expose it meaningfully. */
static inline long parallel_cores_available(void) {
    long total = hermes_parallel_hw_concurrency();
    long active = atomic_load(&hermes_parallel_active_threads);
    long avail = total - active;
    return avail > 0 ? avail : 1;
}

typedef struct {
    long start, end;
    hermes_parallel_body_fn body;
} hermes_parallel_chunk_t;

static void *hermes_parallel_worker(void *arg) {
    hermes_parallel_chunk_t *chunk = (hermes_parallel_chunk_t *)arg;
    for (long i = chunk->start; i < chunk->end; i++) chunk->body(i);
    return NULL;
}

/* Splits [start, end) into `num_threads` contiguous chunks (static
 * partitioning -- no work-stealing in v0.1, fine as long as iterations
 * cost roughly the same), runs each chunk on its own real OS thread, and
 * blocks until all of them finish (synchronous fork-join; no separate
 * "run" step needed, unlike the cooperative hermes_threads API).
 * num_threads <= 0 means "use parallel_cores_available()". */
static inline void parallel_for(long start, long end, hermes_parallel_body_fn body, long num_threads) {
    long range = end - start;
    if (range <= 0) return;

    if (num_threads <= 0) num_threads = parallel_cores_available();
    if (num_threads > range) num_threads = range;
    if (num_threads < 1) num_threads = 1;

    pthread_t *tids = malloc(sizeof(pthread_t) * (size_t)num_threads);
    hermes_parallel_chunk_t *chunks = malloc(sizeof(hermes_parallel_chunk_t) * (size_t)num_threads);
    char *created = calloc((size_t)num_threads, sizeof(char));
    if (!tids || !chunks || !created) {
        fprintf(stderr, "parallel_for: sin memoria, ejecutando secuencialmente\n");
        free(tids); free(chunks); free(created);
        for (long i = start; i < end; i++) body(i);
        return;
    }

    long base = range / num_threads;
    long rem = range % num_threads;
    long cursor = start;

    for (long t = 0; t < num_threads; t++) {
        long chunk_size = base + (t < rem ? 1 : 0);
        chunks[t].start = cursor;
        chunks[t].end = cursor + chunk_size;
        chunks[t].body = body;
        cursor += chunk_size;

        atomic_fetch_add(&hermes_parallel_active_threads, 1);
        if (pthread_create(&tids[t], NULL, hermes_parallel_worker, &chunks[t]) == 0) {
            created[t] = 1;
        } else {
            atomic_fetch_sub(&hermes_parallel_active_threads, 1);
            fprintf(stderr, "parallel_for: fallo al crear un hilo, ejecutando ese tramo aqui mismo\n");
            hermes_parallel_worker(&chunks[t]); /* run this chunk synchronously instead */
        }
    }

    for (long t = 0; t < num_threads; t++) {
        if (created[t]) {
            pthread_join(tids[t], NULL);
            atomic_fetch_sub(&hermes_parallel_active_threads, 1);
        }
    }

    free(tids); free(chunks); free(created);
}

#endif /* __AVR__ */

#endif
