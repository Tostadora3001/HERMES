#ifndef HERMES_DRV_TIMERS_H
#define HERMES_DRV_TIMERS_H

/* Core driver: who uses each hardware timer -- its ROLE. Common code only
 * (no registers): the timer driver (the system tick) and the PWM driver
 * ask it which timer does what, and the `timer` device (src/framework/
 * devices/hermes_timers.h, `import hermes_timers`) changes it. See
 * docs/raw/hermes_timers.en.md.
 *
 * Roles:
 *   system    counts millis/micros and gives the OS its tick (1 ms
 *             interrupt). At most one timer: giving the role to another
 *             takes it from the previous one, which becomes free. With no
 *             system timer, millis() stops (and delay_ms never ends).
 *   pwm       its output compares drive pwm() pins.
 *   reserved  held by someone (today the user; a servo driver later):
 *             nobody else configures it.
 *   free      nobody uses it.
 * Default: Timer0 system, the rest free (a pwm() on a free timer makes it
 * pwm). A role can always be changed to any other: what the hardware
 * allows is the programmer's business (decided with the user) -- this
 * registry only keeps the drivers consistent with each other.
 *
 * Timers = the board's (boards/<board>/hermes_board.h,
 * HERMES_BOARD_TIMERS(X): X(number, bits)), numbered 0..N-1. A board
 * without the list has no timers to manage. Not thread-safe by itself:
 * callers hold the framework lock. */

#include "hermes_platform.h"
#include "hermes_board.h"
#include "hermes_diag.h" /* HERMES_PSTR */

#ifndef HERMES_BOARD_TIMERS
#define HERMES_BOARD_TIMERS(X) /* this board lists no timers */
#endif

#define HERMES_TIMERS_COUNT_ONE_(n, bits) +1
enum { HERMES_TIMERS_COUNT = 0 HERMES_BOARD_TIMERS(HERMES_TIMERS_COUNT_ONE_) };

typedef enum {
    HERMES_TIMER_FREE = 0,
    HERMES_TIMER_SYSTEM,
    HERMES_TIMER_PWM,
    HERMES_TIMER_RESERVED,
} hermes_timer_role_t;

/* who reserved a timer */
typedef enum {
    HERMES_TIMER_OWNER_NONE = 0,
    HERMES_TIMER_OWNER_USER,
} hermes_timer_owner_t;

int hermes_timers_exists(int n);
int hermes_timers_bits(int n);                 /* 8 or 16; 0 if no such timer */
hermes_timer_role_t hermes_timers_role(int n); /* FREE for a timer that doesn't exist */
const char *hermes_timers_role_name(hermes_timer_role_t r);
/* The same in flash on AVR (HERMES_PSTR: print it with "%" HERMES_PS in a
 * HERMES_DIAG message); in RAM elsewhere. role_name is for print(). */
const char *hermes_timers_role_pname(hermes_timer_role_t r);
int hermes_timers_system(void);                /* the system timer, -1 = none */
hermes_timer_owner_t hermes_timers_owner(int n);
const char *hermes_timers_owner_name(hermes_timer_owner_t o);

/* Gives timer n role r (owner: for RESERVED). Side effects, through weak
 * references (each driver may not be linked):
 *   leaving PWM      -> hermes_pwm_timer_lost(n): its pins go LOW and off;
 *   becoming SYSTEM  -> the previous system timer becomes FREE, and
 *                       hermes_timer_hw_system(n) moves millis/the tick;
 *   leaving SYSTEM   -> hermes_timer_hw_system(-1): millis stops. */
void hermes_timers_set_role(int n, hermes_timer_role_t r, hermes_timer_owner_t owner);

/* Implemented by the drivers that care (weak references here). */
void hermes_pwm_timer_lost(int n);
void hermes_timer_hw_system(int n);

#endif
