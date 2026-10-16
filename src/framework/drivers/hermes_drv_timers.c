/* Timer roles, common part (every target). See hermes_drv_timers.h. */
#include "hermes_drv_timers.h"

#ifdef HERMES_TIMERS
/* Weak: NULL when the PWM / timer driver isn't linked. */
extern void hermes_pwm_timer_lost(int n) __attribute__((weak));
extern void hermes_timer_hw_system(int n) __attribute__((weak));
#endif

#define HERMES_TIMERS_SLOTS (HERMES_TIMERS_COUNT > 0 ? HERMES_TIMERS_COUNT : 1)
static unsigned char g_role[HERMES_TIMERS_SLOTS] = { [0] = HERMES_TIMER_SYSTEM };
static unsigned char g_owner[HERMES_TIMERS_SLOTS];

int hermes_timers_exists(int n) { return n >= 0 && n < HERMES_TIMERS_COUNT; }

int hermes_timers_bits(int n) {
#define HERMES_TIMERS_BITS_(num, bits) if (n == (num)) return (bits);
    HERMES_BOARD_TIMERS(HERMES_TIMERS_BITS_)
#undef HERMES_TIMERS_BITS_
    (void)n;
    return 0;
}

hermes_timer_role_t hermes_timers_role(int n) {
    return hermes_timers_exists(n) ? (hermes_timer_role_t)g_role[n] : HERMES_TIMER_FREE;
}

const char *hermes_timers_role_name(hermes_timer_role_t r) {
    switch (r) {
    case HERMES_TIMER_SYSTEM: return "system";
    case HERMES_TIMER_PWM: return "pwm";
    case HERMES_TIMER_RESERVED: return "reserved";
    default: return "free";
    }
}

const char *hermes_timers_role_pname(hermes_timer_role_t r) {
    switch (r) {
    case HERMES_TIMER_SYSTEM: return HERMES_PSTR("system");
    case HERMES_TIMER_PWM: return HERMES_PSTR("pwm");
    case HERMES_TIMER_RESERVED: return HERMES_PSTR("reserved");
    default: return HERMES_PSTR("free");
    }
}

int hermes_timers_system(void) {
    for (int n = 0; n < HERMES_TIMERS_COUNT; n++)
        if (g_role[n] == HERMES_TIMER_SYSTEM) return n;
    return -1;
}

hermes_timer_owner_t hermes_timers_owner(int n) {
    return hermes_timers_exists(n) ? (hermes_timer_owner_t)g_owner[n] : HERMES_TIMER_OWNER_NONE;
}

const char *hermes_timers_owner_name(hermes_timer_owner_t o) {
    return o == HERMES_TIMER_OWNER_USER ? "user" : "none";
}

/* Without `import hermes_timers` (no -DHERMES_TIMERS) the only change is
 * pwm() taking a free timer: no side effects to run, and their code (the
 * PWM driver's timer_lost, the timer driver's move) isn't linked. */
void hermes_timers_set_role(int n, hermes_timer_role_t r, hermes_timer_owner_t owner) {
    if (!hermes_timers_exists(n)) return;
#ifndef HERMES_TIMERS
    (void)owner;
    g_role[n] = (unsigned char)r;
#else
    hermes_timer_role_t old = (hermes_timer_role_t)g_role[n];
    g_owner[n] = (unsigned char)(r == HERMES_TIMER_RESERVED ? owner : HERMES_TIMER_OWNER_NONE);
    if (old == r) return;
    if (old == HERMES_TIMER_PWM && hermes_pwm_timer_lost) hermes_pwm_timer_lost(n);
    if (r == HERMES_TIMER_SYSTEM) {
        int prev = hermes_timers_system();
        if (prev >= 0) g_role[prev] = HERMES_TIMER_FREE;
        g_role[n] = HERMES_TIMER_SYSTEM;
        if (hermes_timer_hw_system) hermes_timer_hw_system(n);
        return;
    }
    g_role[n] = (unsigned char)r;
    if (old == HERMES_TIMER_SYSTEM && hermes_timer_hw_system) hermes_timer_hw_system(-1);
#endif
}
