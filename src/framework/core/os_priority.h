#ifndef OS_PRIORITY_H
#define OS_PRIORITY_H

/* `import os_priority`: the priority scheduler of the OS library
 * (hermes_os). Requires `import hermes_os` written by the user. Nothing to
 * call from HERMESPY: importing it is what selects it; priorities are set
 * with t.priority(p) (hermes_os.h). See docs/raw/hermes_os.en.md.
 *
 * Policy: the ready thread with the highest priority (7 most urgent, 0
 * least) runs. A ready thread of higher priority takes the core from the
 * running one at the OS tick. Among equal priorities, a fair share (the
 * priorities never change): the OS keeps a record of the CPU time each
 * thread used (ms the tick found it on a core, divided by its share) and
 * the least-used one runs. The running one is looked at again every
 * os_slice_ms() ms (default 10): an equal that has used no more takes the
 * core. Only time spent asking for the CPU counts: a thread back from
 * sleeping, a block or a pause starts level with the least-used of its
 * equals. Rounded up to the scheduler's period (os_sched_ticks(): 10 ms
 * by default). No aging between levels: a low priority thread doesn't
 * run while a more urgent one is always ready. See
 * docs/raw/hermes_os.en.md.
 *
 *   os_slice_ms(ms)   how long a thread keeps the core before its equals
 *                     are looked at again, 0-65535 ms; 0 = only when it
 *                     yields or blocks. Only while the OS is stopped
 *   t.share(n)        its share among equals, 1-255 (default 1: all
 *                     alike): with share 3 it gets three times the CPU of
 *                     an equal with 1, while both want it */

#include "hermes_thread.h"

void hermes_os_share(hermes_thread_t *t, long n);
static inline void device_thread_share(hermes_thread_handle_t h_t, long n) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "share");
    hermes_os_share(t, n);
}
static inline void thread_share(hermes_thread_handle_t h_t, long n) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "share");
    hermes_os_share(t, n);
}

void os_slice_ms(long ms);

#endif
