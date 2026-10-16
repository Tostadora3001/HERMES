#include "hermes_parallel.h"
#include "hermes_sched_internal.h"
#include <stdint.h>

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on AVR */

typedef struct {
    hermes_parallel_chunk_fn chunk;
    void *ctx;
    long lo, hi, step;
    hermes_thread_t *thread;
} hermes_parallel_job_t;

/* The thread's `long` argument carries the job's address (a pointer fits a
 * long on both targets: 64/64 bits on the host, 16/32 on AVR). */
static void job_entry(long arg) {
    hermes_parallel_job_t *job = (hermes_parallel_job_t *)(intptr_t)arg;
    job->chunk(job->lo, job->hi, job->step, job->ctx);
}

long parallel_cores_available(void) {
    return hermes_cores_free();
}

long hermes_parallel_auto_threads(void) {
    long n = hermes_cores_free() / 2 * 2;
    return n < 2 ? 2 : n;
}

void hermes_parallel_lock(void) { hermes_lock(); }
void hermes_parallel_unlock(void) { hermes_unlock(); }

static hermes_thread_t *job_thread(hermes_parallel_job_t *job) {
    /* no heap of its own (a chunk never uses one): the whole default block
     * of the profile is stack */
    hermes_thread_attr_t attr = {
        HERMES_THREAD_NO_HEAP,
        HERMES_THREAD_DEFAULT_HEAP_SIZE + HERMES_THREAD_DEFAULT_STACK_SIZE
    };
    return hermes_thread_create_ex(job_entry, (long)(intptr_t)job, &attr);
}

void hermes_parallel_run(long start, long end, long step, long num_threads,
                         hermes_parallel_chunk_fn chunk, void *ctx) {
    if (step <= 0) {
        HERMES_DIAG("parallel for: the step must be positive (it is %ld)\n", step);
        hermes_fatal();
    }
    if (end <= start) return;
    long iters = (end - start + step - 1) / step;
    long n = num_threads > 0 ? num_threads : hermes_parallel_auto_threads();
    if (n > iters) n = iters; /* never an empty chunk */

    hermes_parallel_job_t *jobs = hermes_runtime_alloc(sizeof(hermes_parallel_job_t) * (size_t)n);
    if (!jobs) {
        HERMES_DIAG("parallel for: out of memory for %ld threads\n", n);
        hermes_fatal();
    }
    long base = iters / n, rem = iters % n, first = 0;
    for (long k = 0; k < n; k++) {
        long count = base + (k < rem ? 1 : 0);
        jobs[k].chunk = chunk;
        jobs[k].ctx = ctx;
        jobs[k].lo = start + first * step;
        jobs[k].hi = start + (first + count) * step;
        jobs[k].step = step;
        first += count;
    }

    if (hermes_os_hooks) {
        /* under hermes_os: the threads join the process and the scheduler
         * spreads them over the cores; the caller (a process thread)
         * blocks in wait() meanwhile */
        hermes_thread_t *self = hermes_thread_current();
        if (!hermes_os_hooks->running || !self->in_process) {
            HERMES_DIAG("parallel for: with hermes_os it can only be used inside the "
                        "process's threads (while os_run() runs)\n");
            hermes_fatal();
        }
        for (long k = 0; k < n; k++) jobs[k].thread = job_thread(&jobs[k]);
        for (long k = 0; k < n; k++) {
            hermes_thread_wait(jobs[k].thread);
            hermes_thread_destroy(jobs[k].thread);
        }
        hermes_runtime_free(jobs);
        return;
    }

    if (hermes_cores_count() == 1) {
        /* one core: the caller runs every thread itself, one at a time --
         * only one thread's memory is alive at once (it matters on AVR) */
        for (long k = 0; k < n; k++) {
            hermes_thread_t *t = job_thread(&jobs[k]);
            hermes_thread_run(t);
            hermes_thread_run_to_end(t);
            hermes_thread_destroy(t);
        }
        hermes_runtime_free(jobs);
        return;
    }

    for (long k = 0; k < n; k++) {
        jobs[k].thread = job_thread(&jobs[k]);
        hermes_thread_launch(jobs[k].thread);
    }
    /* the caller's core is a resource too: take whatever no core picked up */
    for (long k = 0; k < n; k++) {
        if (hermes_thread_take(jobs[k].thread)) hermes_thread_run_to_end(jobs[k].thread);
    }
    for (long k = 0; k < n; k++) {
        hermes_thread_wait(jobs[k].thread);
        hermes_thread_destroy(jobs[k].thread);
    }
    hermes_runtime_free(jobs);
}

/* parallel_for(start, end, fn, n): one call of `fn` per index. */
typedef struct { hermes_parallel_body_fn body; } hermes_parallel_for_ctx_t;

static void for_chunk(long lo, long hi, long step, void *ctx) {
    hermes_parallel_body_fn body = ((hermes_parallel_for_ctx_t *)ctx)->body;
    for (long i = lo; i < hi; i += step) body(i);
}

void parallel_for(long start, long end, hermes_parallel_body_fn body, long num_threads) {
    hermes_parallel_for_ctx_t ctx = { body };
    hermes_parallel_run(start, end, 1, num_threads, for_chunk, &ctx);
}
