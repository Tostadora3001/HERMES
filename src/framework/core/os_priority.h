#ifndef OS_PRIORITY_H
#define OS_PRIORITY_H

/* `import os_priority`: the priority scheduler of the OS library
 * (hermes_os). Requires `import hermes_os` written by the user. Nothing to
 * call from HERMESPY: importing it is what selects it; priorities are set
 * with t.priority(p) (hermes_os.h). See docs/raw/hermes_os.en.md.
 *
 * Policy: the ready thread with the highest priority (7 most urgent, 0
 * least) runs; equal priorities take turns (FIFO: a thread that yields
 * goes behind the others of its level). Cooperative: a running thread
 * keeps its core until it yields, blocks or finishes. No aging: a low
 * priority thread doesn't run while a more urgent one is always ready. */

#endif
