#include "hermes_os.h"
#include "hermes_os_sched.h"
#include "hermes_sched_internal.h"
#include "hermes_drv_timer.h" /* the tick: hermes_timer_set_tick */
#include <stddef.h>

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on bare metal */

/* --- the process -----------------------------------------------------------
 * One for now. It's a device (kind "process"), so like every device it has
 * a mandatory abort: stop every thread of the process. Its memory is a
 * list of regions; each region holds the blocks of several threads. */

typedef struct os_free {
    size_t size; /* whole block, header included -- also in used blocks */
    struct os_free *next;
} os_free_t;

typedef struct os_region {
    struct os_region *next;
    size_t size;            /* bytes available for blocks (after this header) */
    unsigned short blocks;  /* blocks in use */
    os_free_t *free;        /* free holes, sorted by address */
} os_region_t;

#define OS_ALIGN ((size_t)_Alignof(max_align_t))
#define OS_ROUND(n) (((n) + OS_ALIGN - 1) / OS_ALIGN * OS_ALIGN)
#define OS_HDR OS_ROUND(sizeof(size_t))
#define OS_MIN_BLOCK OS_ROUND(sizeof(os_free_t))
#define OS_REGION_HDR OS_ROUND(sizeof(os_region_t))

/* One thread block with the profile's default sizes (hermes_thread.h). */
#define OS_DEFAULT_BLOCK (2 * HERMES_CANARY_BYTES + sizeof(hermes_thread_t) \
                          + HERMES_THREAD_DEFAULT_HEAP_SIZE + HERMES_THREAD_DEFAULT_STACK_SIZE)

#ifndef HERMES_OS_REGION_BLOCKS
#if defined(__AVR__)
#define HERMES_OS_REGION_BLOCKS 2
#else
#define HERMES_OS_REGION_BLOCKS 10
#endif
#endif

static void process_abort_entry(hermes_device_t *d);
static void process_describe_entry(hermes_device_t *d, hermes_device_desc_t *o);
static hermes_device_class_t g_process_class = {
    .name = "process", .max_instances = 1, .abort = process_abort_entry,
    .describe = process_describe_entry
};

static struct {
    hermes_device_t device;  /* first: the process is a device */
    os_region_t *regions;
    int region_blocks;
    int live;                /* threads not finished yet */
} g_proc;

/* Every caller holds the lock exactly once. */
static void fatal(void) {
    hermes_unlock();
    hermes_fatal();
}

/* --- regions ---------------------------------------------------------------- */

static void *take_from(os_region_t *r, size_t need) {
    os_free_t **link = &r->free;
    for (os_free_t *b = r->free; b; link = &b->next, b = b->next) {
        if (b->size < need) continue;
        if (b->size - need >= OS_MIN_BLOCK) { /* split: the tail stays free */
            os_free_t *rest = (os_free_t *)((unsigned char *)b + need);
            rest->size = b->size - need;
            rest->next = b->next;
            *link = rest;
            b->size = need;
        } else {
            *link = b->next;
        }
        r->blocks++;
        return (unsigned char *)b + OS_HDR;
    }
    return NULL;
}

static void *block_alloc(size_t n) {
    size_t need = OS_ROUND(OS_HDR + n);
    if (need < OS_MIN_BLOCK) need = OS_MIN_BLOCK;
    hermes_lock();
    os_region_t *last = NULL;
    for (os_region_t *r = g_proc.regions; r; last = r, r = r->next) {
        void *p = take_from(r, need);
        if (p) { hermes_unlock(); return p; }
    }
    /* full: a new region of N default blocks (or one block's worth, for a
     * thread bigger than that) */
    if (!g_proc.region_blocks) g_proc.region_blocks = HERMES_OS_REGION_BLOCKS;
    size_t size = (size_t)g_proc.region_blocks * OS_ROUND(OS_HDR + OS_DEFAULT_BLOCK);
    if (size < need) size = need;
    os_region_t *r = hermes_runtime_alloc(OS_REGION_HDR + size);
    if (!r) { hermes_unlock(); return NULL; }
    r->next = NULL;
    r->size = size;
    r->blocks = 0;
    r->free = (os_free_t *)((unsigned char *)r + OS_REGION_HDR);
    r->free->size = size;
    r->free->next = NULL;
    if (last) last->next = r;
    else g_proc.regions = r;
    void *p = take_from(r, need);
    hermes_unlock();
    return p;
}

static void block_free(void *p) {
    os_free_t *b = (os_free_t *)((unsigned char *)p - OS_HDR);
    hermes_lock();
    os_region_t *prev_r = NULL, *r = g_proc.regions;
    while (r && !((unsigned char *)b >= (unsigned char *)r + OS_REGION_HDR &&
                  (unsigned char *)b < (unsigned char *)r + OS_REGION_HDR + r->size)) {
        prev_r = r;
        r = r->next;
    }
    if (!r) { hermes_unlock(); return; } /* not ours: can't happen */
    os_free_t *prev = NULL, *next = r->free;
    while (next && next < b) { prev = next; next = next->next; }
    b->next = next;
    if (prev) prev->next = b;
    else r->free = b;
    if (next && (unsigned char *)b + b->size == (unsigned char *)next) {
        b->size += next->size;
        b->next = next->next;
    }
    if (prev && (unsigned char *)prev + prev->size == (unsigned char *)b) {
        prev->size += b->size;
        prev->next = b->next;
    }
    if (--r->blocks == 0) { /* empty: back to the free memory */
        if (prev_r) prev_r->next = r->next;
        else g_proc.regions = r->next;
        hermes_runtime_free(r);
    }
    hermes_unlock();
}

/* --- the tick ---------------------------------------------------------------
 * The timer driver calls os_tick() every millisecond from its interrupt
 * (host: from its tick pthread) while os_run() is in progress. Every
 * os_tick_ms() ms is one OS tick; every os_sched_ticks() OS ticks the
 * scheduler is asked, for each core running a thread, who should be there;
 * if it wants another thread, the running one gives the core back
 * (hermes_sched_request_switch): on bare metal right away, from this same
 * interrupt (preemption); on the host at its next safe point.
 *
 * On every OS tick the interrupt also looks whether the OS has secondary
 * work due -- a sleeping thread to wake, a device kind's tick -- but it
 * doesn't do it (decided with the user): it only activates the OS, whose
 * work runs in a core's scheduler loop (os_work, below), outside the
 * interrupt; the thread on that core gives it up for the moment, as with
 * the scheduler. While the OS works (hermes_os_working), a tick counts --
 * os_ticks(), the ms counter -- but does nothing else: it neither
 * schedules nor activates the OS again. Nothing is lost: what fell due
 * meanwhile is found the next time the OS looks. */

#ifndef HERMES_OS_TICK_MS
#define HERMES_OS_TICK_MS 1
#endif
#ifndef HERMES_OS_SCHED_TICKS
#define HERMES_OS_SCHED_TICKS 10
#endif

static unsigned int g_tick_ms = HERMES_OS_TICK_MS;        /* ms per OS tick */
static unsigned int g_sched_ticks = HERMES_OS_SCHED_TICKS; /* OS ticks per scheduler call */
static unsigned int g_ms_left, g_sched_left;              /* countdowns */
static unsigned long g_ticks;                             /* OS ticks so far */
/* the process thread each core is running (NULL: none) -- set by core_loop */
static hermes_thread_t *g_running[HERMES_MAX_CORES];
static hermes_os_hooks_t g_hooks; /* defined below */

/* The OS's own ms counter (one per call of os_tick: every ms while
 * os_run runs; never reset). Sleeping threads and device ticks count on it. */
static unsigned long g_now_ms;
/* threads asleep in delay_ms (BLOCKED here, in no order) and the earliest
 * wake_at among them (may be early after an abort: harmless) */
static hermes_wait_queue_t g_sleepers;
static unsigned long g_next_wake;
/* the OS has work due (set by the tick, cleared by os_work) */
static volatile unsigned char g_os_due;
/* the thread on that core gave it up only so the OS could work: it gets
 * it back afterwards unless the scheduler wants someone else there */
static volatile unsigned char g_os_bumped[HERMES_MAX_CORES];

volatile unsigned char hermes_os_working = 0;

static int time_reached(unsigned long at) { return (long)(g_now_ms - at) >= 0; }

/* Lock held (or in the tick). */
static int secondary_due(void) {
    return (g_sleepers.head && time_reached(g_next_wake)) || hermes_device_tick_due(g_now_ms) ||
           (HERMES_SCHED(due) && HERMES_SCHED(due)(g_now_ms));
}

/* Lock held: the tick found work for the OS. A core with nothing to run
 * does it (it's woken); if every core runs a thread, core 0's gives it up. */
static void activate_os(void) {
    g_os_due = 1;
    for (int core = 0; core < hermes_cores_count(); core++) {
        if (!g_running[core]) { hermes_sched_notify(); return; }
    }
    g_os_bumped[0] = 1;
    hermes_sched_request_switch(0);
    hermes_sched_notify();
}

/* Lock held. */
static void sched_tick(void) {
    hermes_thread_t *taken[HERMES_MAX_CORES];
    int ntaken = 0;
    for (int core = 0; core < hermes_cores_count(); core++) {
        hermes_thread_t *running = g_running[core];
        if (!running) continue;
        hermes_thread_t *want = HERMES_SCHED(decide)(core, running, taken, ntaken);
        if (want == running) continue;
        if (want) taken[ntaken++] = want;
        g_os_bumped[core] = 0; /* it leaves for the scheduler, not only for the OS */
        hermes_sched_request_switch(core);
    }
}

/* Interrupt context (bare metal: interrupts masked, so the lock is free --
 * nobody can hold it while an interrupt runs -- and taking it changes
 * nothing). The switch, if any, is done with the lock released: the
 * thread leaves its core inside this call and comes back to it when it's
 * resumed. */
static void os_tick(void) {
    hermes_lock();
    if (g_hooks.running) {
        g_now_ms++;
        if (HERMES_SCHED(charge)) /* os_deadline: each job's used ms */
            for (int core = 0; core < hermes_cores_count(); core++)
                if (g_running[core]) HERMES_SCHED(charge)(g_running[core]);
    }
    if (g_hooks.running && --g_ms_left == 0) {
        g_ms_left = g_tick_ms;
        g_ticks++;
        int act = !hermes_os_working; /* while the OS works, a tick only counts */
        if (act && !g_os_due && secondary_due()) activate_os();
        if (--g_sched_left == 0) {
            g_sched_left = g_sched_ticks;
            if (act) sched_tick();
        }
    }
    hermes_unlock();
    hermes_sched_preempt();
}

/* --- hooks for the thread library ------------------------------------------ */

static void created(hermes_thread_t *t) {
    g_proc.live++;
    if (HERMES_SCHED(created)) HERMES_SCHED(created)(t);
    HERMES_SCHED(ready)(t);
    hermes_sched_notify();
}

static void ready(hermes_thread_t *t) {
    HERMES_SCHED(ready)(t);
    hermes_sched_notify();
}

static void terminated(hermes_thread_t *t) {
    (void)t;
    g_proc.live--;
    hermes_sched_notify();
}

/* Its effective priority changed (mutex priority inheritance): waiting in
 * the ready set, it moves to its new level. */
static void reprioritized(hermes_thread_t *t) {
    if (t->in_process && t->state == HERMES_THREAD_READY && t->queue) {
        hermes_queue_unlink(t);
        HERMES_SCHED(ready)(t);
    }
    hermes_sched_notify();
}

static int time_will_wake(void) { return g_sleepers.head != NULL; }

static void core_loop(void);

static hermes_os_hooks_t g_hooks = {
    .block_alloc = block_alloc,
    .block_free = block_free,
    .created = created,
    .ready = ready,
    .terminated = terminated,
    .core_loop = core_loop,
    .reprioritized = reprioritized,
    .time_will_wake = time_will_wake,
    .running = 0,
};

__attribute__((constructor))
static void hermes_os_bootstrap(void) {
    hermes_os_hooks = &g_hooks;
    hermes_device_register_or_abort(&g_proc.device, &g_process_class);
}

/* --- the OS's work -----------------------------------------------------------
 * Done by a core's scheduler loop -- the OS's own context, not a thread of
 * the process (it needs no thread block: decided with the user) -- when
 * the tick activated it. Lock held; released while device kinds run their
 * tick, with hermes_os_working set (the tick then only counts). Neither
 * may block or yield: there's no thread here to block. */
static void os_work(void) {
    hermes_os_working = 1;
    g_os_due = 0;
    unsigned long now = g_now_ms;
    /* wake every sleeper whose time came; the earliest of the rest */
    hermes_thread_t *t = g_sleepers.head;
    int any = 0, woke = 0;
    while (t) {
        hermes_thread_t *next = t->queue_next;
        if ((long)(now - t->wake_at) >= 0) {
            hermes_sched_wake(t);
            woke = 1;
        } else if (!any || (long)(t->wake_at - g_next_wake) < 0) {
            g_next_wake = t->wake_at;
            any = 1;
        }
        t = next;
    }
    /* a thread just woken may be more urgent than one running on another
     * core (e.g. pinned to it): the scheduler is asked now, not at its
     * next period (os_deadline: a short deadline would be missed) */
    if (woke) sched_tick();
    /* the scheduler's own (os_deadline: missed deadlines) */
    if (HERMES_SCHED(work)) HERMES_SCHED(work)(now);
    hermes_unlock();
    hermes_device_tick_run(now);
    hermes_lock();
    hermes_os_working = 0;
}

/* --- the scheduler loop ----------------------------------------------------
 * Every core runs it while os_run() is in progress (core 0 inside os_run,
 * the others from their idle loop), lock held. The ready set is shared by
 * all cores, so a free core takes the most urgent ready thread wherever it
 * ran last: that's the automatic migration. pin/migrate restrict it.
 * It's also where the OS works (os_work), before choosing the next thread. */
static void core_loop(void) {
    int core = hermes_core_current();
    hermes_thread_t *bumped = NULL; /* gave the core up only for the OS */
    while (g_hooks.running) {
        if (g_os_due && !hermes_os_working) os_work();
        hermes_thread_t *t = NULL;
        if (bumped) {
            /* back to it, unless it can't go on or the scheduler (asked as
             * on a tick) wants another thread here */
            t = bumped;
            bumped = NULL;
            t->on_cpu = 0;
            hermes_sched_notify(); /* a destroy() may have been waiting for it */
            if (t->state != HERMES_THREAD_READY || t->paused) {
                t = NULL;
            } else if (HERMES_SCHED(decide)(core, t, NULL, 0) != t) {
                HERMES_SCHED(ready)(t);
                t = NULL;
            }
        }
        if (g_proc.live == 0) {
            g_hooks.running = 0;
            hermes_sched_notify();
            break;
        }
        if (!t) t = HERMES_SCHED(pick)(core);
        if (!t) { hermes_sched_idle(); continue; }
        t->core = (signed char)core;
        t->next_core = -1; /* a migrate() is used up */
        g_running[core] = t;
        g_os_bumped[core] = 0;
        hermes_sched_resume(t);
        g_running[core] = NULL;
        if (t->state == HERMES_THREAD_READY && !t->paused && !t->queue && g_os_bumped[core] &&
            !hermes_sched_switch_pending(core) /* it really left on the request, not by yielding */) {
            /* it left only so the OS could work: kept aside -- on no other
             * core, and not released by a destroy() (on_cpu) -- to get the
             * core back afterwards, ahead of its equals */
            g_os_bumped[core] = 0;
            t->on_cpu = 1;
            bumped = t;
            continue;
        }
        /* it yielded: back to the ready set (blocked ones come back when
         * woken; finished ones are gone) */
        if (t->state == HERMES_THREAD_READY && !t->paused && !t->queue) HERMES_SCHED(ready)(t);
        else if (HERMES_SCHED(left)) HERMES_SCHED(left)(t);
    }
}

/* --- sleeping (delay_ms) -----------------------------------------------------
 * The timer driver's delay_ms calls this (weak reference) before waiting:
 * a process thread, while the OS runs, sleeps -- BLOCKED until the OS
 * wakes it -- instead of yielding again and again asking for the time.
 * Returns 0 (not done: the caller waits its own way) anywhere else. */
int hermes_os_sleep_ms(unsigned long ms) {
    hermes_lock();
    hermes_thread_t *self = hermes_thread_current();
    if (!g_hooks.running || !self->in_process || ms == 0) {
        hermes_unlock();
        return 0;
    }
    hermes_os_sleep_until_locked(g_now_ms + ms);
    return 1;
}

void hermes_os_sleep_until_locked(unsigned long at) {
    hermes_thread_t *self = hermes_thread_current();
    self->wake_at = at;
    if (!g_sleepers.head || (long)(at - g_next_wake) < 0) g_next_wake = at;
    hermes_sched_block_current(&g_sleepers); /* returns, unlocked, once woken */
}

unsigned long hermes_os_now_ms(void) { return g_now_ms; }

/* --- HERMESPY-facing --------------------------------------------------------- */

void os_region_blocks(long n) {
    hermes_lock();
    if (g_proc.regions) {
        HERMES_DIAG("hermes_os: os_region_blocks() after creating threads (the process already "
                    "has regions): call it before creating any\n");
        fatal();
    }
    g_proc.region_blocks = n > 0 ? (int)n : HERMES_OS_REGION_BLOCKS;
    hermes_unlock();
}

void os_run(void) {
    hermes_lock();
    hermes_thread_t *self = hermes_thread_current();
    if (self->in_process) {
        HERMES_DIAG("hermes_os: os_run() only from main\n");
        fatal();
    }
    if (g_hooks.running) { hermes_unlock(); return; }
    if (HERMES_SCHED(start)) HERMES_SCHED(start)(); /* os_deadline: periods start now */
    g_ms_left = g_tick_ms;
    g_sched_left = g_sched_ticks;
    g_hooks.running = 1;
    hermes_device_ticks_on = 1;
    hermes_unlock();
    /* outside the lock: on bare metal it enables interrupts (the lock
     * would put back "disabled" when released); on the host the tick
     * pthread takes the lock */
    hermes_timer_set_tick(os_tick);
    hermes_lock();
    hermes_cores_start();
    hermes_sched_notify();
    core_loop();
    hermes_device_ticks_on = 0;
    hermes_unlock();
    hermes_timer_set_tick(NULL); /* the OS's tick stops; millis goes on */
}

/* Both only while the OS is stopped (before os_run, or after it returns). */
static void set_while_stopped(unsigned int *what, long n, const char *name) {
    hermes_lock();
    if (g_hooks.running) {
        HERMES_DIAG("hermes_os: %" HERMES_PS "() while the OS is running: call it before os_run()\n", name);
        (void)name;
        fatal();
    }
    if (n < 1 || n > 65535) {
        HERMES_DIAG("hermes_os: %" HERMES_PS "(%ld): must be between 1 and 65535\n", name, n);
        fatal();
    }
    *what = (unsigned int)n;
    hermes_unlock();
}

void os_tick_ms(long ms) { set_while_stopped(&g_tick_ms, ms, HERMES_PSTR("os_tick_ms")); }
void os_sched_ticks(long ticks) { set_while_stopped(&g_sched_ticks, ticks, HERMES_PSTR("os_sched_ticks")); }

long os_ticks(void) {
    hermes_lock();
    long n = (long)g_ticks;
    hermes_unlock();
    return n;
}

/* Nestable; the counter belongs to the calling thread (255 levels). */
void os_preempt_off(void) {
    if (hermes_thread_current()->no_preempt == 255) {
        HERMES_DIAG("hermes_os: os_preempt_off() nested too deep (255 levels)\n");
        hermes_fatal();
    }
    hermes_preempt_off();
}

void os_preempt_on(void) {
    if (hermes_thread_current()->no_preempt == 0) {
        HERMES_DIAG("hermes_os: os_preempt_on() without os_preempt_off()\n");
        hermes_fatal();
    }
    hermes_preempt_on();
}

long os_threads(void) {
    hermes_lock();
    long n = g_proc.live;
    hermes_unlock();
    return n;
}

long os_regions(void) {
    long n = 0;
    hermes_lock();
    for (os_region_t *r = g_proc.regions; r; r = r->next) n++;
    hermes_unlock();
    return n;
}

long os_memory(void) {
    long n = 0;
    hermes_lock();
    for (os_region_t *r = g_proc.regions; r; r = r->next) n += (long)(OS_REGION_HDR + r->size);
    hermes_unlock();
    return n;
}

static void check_core(long core, const char *op) {
    if (core >= 0 && core < hermes_cores_count()) return;
    HERMES_DIAG("hermes_os: %" HERMES_PS "(%ld): no such core (there are %d)\n", op, core, hermes_cores_count());
    (void)op;
    fatal();
}

/* (hermes_os_set_priority, t.priority(p), is the scheduler's: os_priority
 * sets it, the others refuse it -- so only os_priority pays for it.) */

void hermes_os_migrate(hermes_thread_t *t, long core) {
    hermes_lock();
    hermes_thread_check_live(t, HERMES_PSTR("migrate"));
    check_core(core, HERMES_PSTR("migrate"));
    t->next_core = (signed char)core;
    hermes_sched_notify();
    hermes_unlock();
}

void hermes_os_pin(hermes_thread_t *t, long core) {
    hermes_lock();
    hermes_thread_check_live(t, core < 0 ? HERMES_PSTR("unpin") : HERMES_PSTR("pin"));
    if (core >= 0) check_core(core, HERMES_PSTR("pin"));
    t->pin = (signed char)(core < 0 ? -1 : core);
    hermes_sched_notify();
    hermes_unlock();
}

/* describe (hermes_device.h): its regions, minus its threads' blocks (each
 * thread counts its own: nothing is counted twice) -- headers and free
 * room. Lock held. Only with import hermes_devices: nothing else asks. */
static void process_describe_entry(hermes_device_t *d, hermes_device_desc_t *o) {
    (void)d; (void)o;
#ifdef HERMES_DEVICES /* no pins: only the dump and devices_memory() ask */
    unsigned long bytes = 0;
    long regions = 0;
    for (os_region_t *r = g_proc.regions; r; r = r->next) {
        bytes += OS_REGION_HDR + r->size;
        regions++;
    }
    (void)regions; /* (a field of the dump only) */
    for (hermes_device_t *it = hermes_device_class_thread.head; it; it = it->next) {
        hermes_thread_t *t = (hermes_thread_t *)it;
        if (t->in_process && t->block_start && bytes >= t->block_size) bytes -= t->block_size;
    }
    o->bytes = bytes;
    HERMES_DEVICE_VALUE(o, "threads", g_proc.live);
    HERMES_DEVICE_VALUE(o, "regions", regions);
#endif
}

/* The process's abort: every thread of the process, except the caller
 * (it must be able to go on, e.g. to report it). Lock held. */
static void process_abort_entry(hermes_device_t *d) {
    (void)d;
    hermes_thread_t *self = hermes_thread_current();
    for (hermes_device_t *it = hermes_device_class_thread.head; it; it = it->next) {
        hermes_thread_t *t = (hermes_thread_t *)it;
        if (t->in_process && t != self) hermes_thread_abort(t);
    }
}
