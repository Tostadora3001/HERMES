#ifndef OS_CYCLIC_H
#define OS_CYCLIC_H

/* `import os_cyclic`: the cyclic scheduler of the OS library (hermes_os).
 * Requires `import hermes_os` written by the user. Nothing to call from
 * HERMESPY: importing it is what selects it. See docs/raw/hermes_os.en.md.
 *
 * Policy (decided with the user): the threads take turns in a fixed
 * order, the order they were created in, round and round. The running
 * thread keeps its core until it gives it up -- it yields, blocks (a
 * semaphore, a mutex, wait(), delay_ms...), is paused or finishes -- and
 * then the next one in the order that can run goes; a thread that can't
 * run (blocked, paused) is skipped, and it keeps its place for the next
 * round. Time plays no part: the tick never takes the core from a thread
 * (no preemption, not even on bare metal), and there are no priorities
 * (t.priority(p) is a fatal error). With several cores (host), each free
 * core takes the next one in the order. */

#endif
