/* Timer driver, host (native) implementation: CLOCK_MONOTONIC; the tick
 * is a pthread. See hermes_drv_timer.h. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime, clock_nanosleep, nanosleep */
#include "hermes_drv_timer.h"
#include <pthread.h>
#include <time.h>

static struct timespec g_base;
static int g_started = 0;

void hermes_timer_init(void) {
    if (g_started) return;
    g_started = 1;
    clock_gettime(CLOCK_MONOTONIC, &g_base);
}

static uint64_t elapsed_us(void) {
    hermes_timer_init();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)(now.tv_sec - g_base.tv_sec) * 1000000ULL
         + (uint64_t)((now.tv_nsec - g_base.tv_nsec) / 1000);
}

uint32_t hermes_timer_millis(void) {
    return (uint32_t)(elapsed_us() / 1000ULL);
}

uint32_t hermes_timer_micros(void) {
    return (uint32_t)elapsed_us();
}

/* don't spin a real core at 100% while delay_ms waits */
void hermes_timer_idle(void) {
    struct timespec nap = { 0, 100000 }; /* 100 us */
    nanosleep(&nap, NULL);
}

/* --- the tick: a pthread standing in for the timer interrupt ------------- */

static void (*volatile g_tick)(void);
static volatile int g_tick_on = 0;
static pthread_t g_tick_thread;

static void add_ns(struct timespec *t, long ns) {
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; }
}

/* Every ms, on absolute deadlines (no drift). After a long delay (the
 * process was stopped, the machine overloaded) it starts again from now
 * instead of firing every missed tick in a row. */
static void *tick_main(void *arg) {
    (void)arg;
    struct timespec next, now;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (g_tick_on) {
        add_ns(&next, 1000000L);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        if (!g_tick_on) break;
        void (*tick)(void) = g_tick;
        if (tick) tick();
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1) next = now;
    }
    return NULL;
}

void hermes_timer_set_tick(void (*fn)(void)) {
    hermes_timer_init();
    g_tick = fn;
    if (fn && !g_tick_on) {
        g_tick_on = 1;
        if (pthread_create(&g_tick_thread, NULL, tick_main, NULL) != 0) g_tick_on = 0;
    } else if (!fn && g_tick_on) {
        g_tick_on = 0;
        pthread_join(g_tick_thread, NULL);
    }
}
