/* Timer driver, common part (every target): the delays, over each
 * target's millis/micros. See hermes_drv_timer.h. */
#include "hermes_drv_timer.h"

/* Weak: resolves to NULL when the thread runtime (hermes_thread.c) isn't
 * linked -- see the header. */
extern void hermes_thread_yield(void) __attribute__((weak));
/* Weak too: NULL without the OS (hermes_os.c). */
extern int hermes_os_sleep_ms(unsigned long ms) __attribute__((weak));

void hermes_timer_delay_ms(uint32_t ms) {
    uint32_t start = hermes_timer_millis();
    uint32_t done;
    while ((done = hermes_timer_millis() - start) < ms) {
        /* a process thread under the OS sleeps instead (and re-checks the
         * time when woken: the OS counts its own ms) */
        if (hermes_os_sleep_ms && hermes_os_sleep_ms(ms - done)) continue;
        if (hermes_thread_yield) hermes_thread_yield();
        hermes_timer_idle();
    }
}

void hermes_timer_delay_us(uint32_t us) {
    uint32_t start = hermes_timer_micros();
    while (hermes_timer_micros() - start < us) {
    }
}
