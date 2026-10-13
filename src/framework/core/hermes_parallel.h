#ifndef HERMES_PARALLEL_H
#define HERMES_PARALLEL_H

/* Parallel loops over the thread library's cores (hermes_thread.h): the
 * iterations are split into contiguous chunks, one hermes thread per
 * chunk, launched on the free cores; the calling core works too (it takes
 * the chunks no other core has picked up), then waits for the rest.
 * Synchronous: returns when every chunk has finished. On AVR (one core)
 * the caller runs the chunks one after the other.
 *
 * Two ways in:
 *   - HERMESPY syntax (the main one), see docs/raw/hermes_parallel.en.md:
 *
 *         parallel for i in range(0, n) threads 4:
 *             global(+) long total = 0   # shared, combined at the end
 *             global long seen = 0       # shared, protected by the user
 *             local long mine = 0        # one per thread
 *             ...
 *
 *     hepyc turns the body into a chunk function and calls
 *     hermes_parallel_run.
 *   - parallel_for(start, end, fn, num_threads): a plain function per
 *     index, kept from v0.1.
 *
 * Bodies run in hermes threads, so they may use semaphores (sem_wait...)
 * and create devices: everything shared in the framework is locked. */

typedef void (*hermes_parallel_body_fn)(long index);

/* One chunk: iterations lo, lo+step, ... while < hi. `ctx` is whatever the
 * caller passed to hermes_parallel_run. */
typedef void (*hermes_parallel_chunk_fn)(long lo, long hi, long step, void *ctx);

/* Runs [start, end) with `step` (> 0) over `num_threads` threads;
 * num_threads <= 0 = automatic (hermes_parallel_auto_threads). More threads
 * than free cores is fine: they are all created and the extra ones wait
 * for a core. */
void hermes_parallel_run(long start, long end, long step, long num_threads,
                         hermes_parallel_chunk_fn chunk, void *ctx);

/* Automatic thread count: the free cores (counting the caller's), rounded
 * down to an even number, at least 2. */
long hermes_parallel_auto_threads(void);

/* Critical section for generated code (reductions): the framework lock. */
void hermes_parallel_lock(void);
void hermes_parallel_unlock(void);

/* Free cores right now, counting the caller's own (>= 1). */
long parallel_cores_available(void);

void parallel_for(long start, long end, hermes_parallel_body_fn body, long num_threads);

#endif
