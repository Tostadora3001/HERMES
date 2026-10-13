#ifndef HERMES_OS_H
#define HERMES_OS_H

/* `import hermes_os`: the mini OS on top of the thread library. It adds
 * what hermes_threads deliberately lacks -- a scheduler that decides who
 * runs -- plus migration of threads between cores, and a process that
 * holds the blocks of its threads. See docs/raw/hermes_os.en.md.
 *
 * Requires, written by the user (hepyc refuses otherwise):
 *   import hermes_threads     the mechanism
 *   import hermes_os          this
 *   import os_priority        the scheduler (one scheduler import)
 *
 * With it imported:
 *   - every thread created belongs to the (single) process: its block is
 *     placed in the process's regions, and it enters the scheduler;
 *   - nothing runs until main calls os_run(), which returns when every
 *     thread of the process has finished;
 *   - run()/launch()/launch_on() on a process thread are a fatal error
 *     (the scheduler decides); wait() from a process thread blocks it;
 *   - while os_run() runs, the timer's interrupt (Timer0 on AVR, SYSTIMER
 *     on the ESP32-C3, a pthread on the host) calls the OS every ms: the
 *     tick. Every os_sched_ticks() ticks the scheduler says who should be
 *     on each core, and a thread it wants out gives the core back:
 *       bare metal (AVR, ESP32-C3): right away, wherever it is -- the
 *         tick's interrupt takes the core from it (preemption) and it
 *         carries on from that same instruction when it's resumed;
 *       host: the next time it enters the framework (thread_yield, a
 *         semaphore, delay_ms, any thread/device/OS function...); a loop
 *         that never calls the framework keeps its core;
 *     never between os_preempt_off() and os_preempt_on(), nor in the
 *     middle of a print() line.
 *
 *   os_region_blocks(n)   thread blocks per region (call before creating
 *                         threads; 0 = default: 10 host, 2 AVR)
 *   os_tick_ms(n)         ms per OS tick (default 1)          } only while the
 *   os_sched_ticks(n)     OS ticks per scheduler call (10)    } OS is stopped
 *   os_run()              start the scheduler on every core (from main)
 *   os_ticks()            OS ticks so far (they count while os_run runs)
 *   os_preempt_off()      the calling thread keeps its core until the
 *   os_preempt_on()       matching on() (nestable; a switch the scheduler
 *                         asked for meanwhile happens then)
 *   os_threads()          process threads not finished yet
 *   os_regions(), os_memory()   regions and bytes the process holds
 *   t.priority(p)         0-7, higher = more urgent (default 4)
 *   t.migrate(n)          run on core n next time
 *   t.pin(n) / t.unpin()  always on core n / anywhere
 */

#include "hermes_thread.h"

void os_region_blocks(long n);
void os_tick_ms(long ms);
void os_sched_ticks(long ticks);
void os_run(void);
long os_ticks(void);
void os_preempt_off(void);
void os_preempt_on(void);
long os_threads(void);
long os_regions(void);
long os_memory(void);

void hermes_os_set_priority(hermes_thread_t *t, long p);
void hermes_os_migrate(hermes_thread_t *t, long core);
void hermes_os_pin(hermes_thread_t *t, long core);

/* methods on a thread (`device t = thread_start(...)`) ... */
static inline void device_thread_priority(hermes_thread_t *t, long p) { hermes_os_set_priority(t, p); }
static inline void device_thread_migrate(hermes_thread_t *t, long core) { hermes_os_migrate(t, core); }
static inline void device_thread_pin(hermes_thread_t *t, long core) { hermes_os_pin(t, core); }
static inline void device_thread_unpin(hermes_thread_t *t) { hermes_os_pin(t, -1); }

/* ... and the same as functions (for hermes_thread_handle_t values) */
static inline void thread_priority(hermes_thread_t *t, long p) { hermes_os_set_priority(t, p); }
static inline void thread_migrate(hermes_thread_t *t, long core) { hermes_os_migrate(t, core); }
static inline void thread_pin(hermes_thread_t *t, long core) { hermes_os_pin(t, core); }
static inline void thread_unpin(hermes_thread_t *t) { hermes_os_pin(t, -1); }

#endif
