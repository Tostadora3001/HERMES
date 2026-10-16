#ifndef HERMES_DRV_PWM_H
#define HERMES_DRV_PWM_H

/* Core driver: PWM outputs. The only place in HERMES that configures the
 * timers' PWM -- the `pwm` device (src/framework/devices/hermes_pwm.h,
 * `import hermes_pwm`) goes through this.
 *
 * Common interface; one implementation per platform:
 *   hermes_drv_pwm.c                   common part: pins, timer ownership,
 *                                      duty and frequency arithmetic
 *   avr/hermes_drv_pwm_avr.c           megaAVR timers' registers
 *   native/hermes_drv_pwm_native.c     host mock
 * (No ESP32-C3 implementation yet: hepyc says so.)
 *
 * Pins = the board's numbering; the board lists which timer output drives
 * each PWM pin (HERMES_BOARD_PWM_PINS: pin, timer, channel, bits).
 *
 * Duty 0-255 (0 = always LOW, 255 = always HIGH). Frequency: 490 Hz by
 * default (phase-correct PWM, prescaler 64, as Arduino); hermes_pwm_freq
 * changes it. The frequency belongs to the TIMER: every pin on the same
 * timer changes with it (each keeps its duty) -- Uno: D9/D10 (Timer1),
 * D3/D11 (Timer2).
 *   16-bit timers (Timer1; Timer3-5 on the Mega): TOP in ICRn, so any
 *     frequency from 1 Hz up to F_CPU/6 is reachable within <1 %
 *     (the duty keeps 256 steps while TOP >= 255, i.e. up to ~31 kHz,
 *     fewer above);
 *   8-bit Timer2: TOP fixed at 255, only its prescalers: 31372, 3921,
 *     980, 490, 245, 122 or 30 Hz at 16 MHz -- the nearest is taken
 *     (Timer0, if given the pwm role: 31372, 3921, 490, 122, 30).
 * The freq functions return the frequency actually set.
 *
 * Timer roles (hermes_drv_timers.h): PWM uses a timer only while its
 * role is `pwm`. A pin whose timer is free makes it pwm; a pin whose timer
 * is `system` (Timer0 by default: Uno D5/D6) or `reserved` is refused.
 * When a timer stops being pwm (import hermes_timers), its pins go LOW and
 * off; writing them is an error until the timer is pwm again. Timer0 can
 * do PWM too once it's given the role (8 bits, prescalers 1, 8, 64, 256,
 * 1024: 31372, 3921, 490, 122 or 30 Hz).
 *
 * Not thread-safe by itself: the device takes the framework lock around
 * every call. Host: a mock that traces each duty written
 * (`[pwm mock] pin N -> duty`). */

#include <stdint.h>
#include "hermes_platform.h"
#include "hermes_board.h"

#ifndef HERMES_BOARD_PWM_PINS
#define HERMES_BOARD_PWM_PINS(X) /* this board has no PWM outputs */
#endif

#define HERMES_PWM_COUNT_ONE_(pin, timer, ch, bits) +1
enum { HERMES_PWM_PIN_COUNT = 0 HERMES_BOARD_PWM_PINS(HERMES_PWM_COUNT_ONE_) };

typedef enum {
    HERMES_PWM_OK = 0,
    HERMES_PWM_ERR_NO_PWM, /* the pin has no PWM output on this board */
    HERMES_PWM_ERR_ROLE,   /* its timer's role isn't pwm (nor free) */
} hermes_pwm_status_t;

/* --- common part --------------------------------------------------------- */

/* Index of `pin` in the board's PWM table, -1 if it has no PWM. */
int hermes_pwm_index(int pin);
int hermes_pwm_timer_of(int pin); /* -1 if no PWM */

/* Starts PWM on `pin` at duty 0 (its timer at its current frequency, or
 * the default one if it's the timer's first pin). */
hermes_pwm_status_t hermes_pwm_start(int pin);
/* Duty 0-255 on a pwm pin (started again if its timer stopped being pwm
 * and is pwm again). */
hermes_pwm_status_t hermes_pwm_write(int pin, uint8_t duty);
/* Frequency of a pwm timer, as near `hz` as it can (hz >= 1). Returns the
 * frequency set, in Hz (rounded), or 0 if `hz` is out of reach (too high:
 * over F_CPU/6 on a 16-bit timer) or the timer isn't pwm. */
long hermes_pwm_timer_freq(int timer, long hz);
long hermes_pwm_timer_freq_now(int timer); /* Hz, rounded (490 until set) */
int hermes_pwm_timer_pins(int timer);      /* pins started on it */
void hermes_pwm_timer_zero(int timer);     /* its pins to duty 0 */
/* The same, by pin (its timer). */
long hermes_pwm_freq(int pin, long hz);
long hermes_pwm_freq_now(int pin);
/* Stops PWM on `pin`: output LOW. */
void hermes_pwm_stop(int pin);
/* The duty last written on `pin` (0 if it isn't running). */
int hermes_pwm_duty_now(int pin);

/* --- implemented by each target (called by the common part) -------------- */

/* The timer's mode, prescaler (`cs`: the chip's clock-select code) and
 * TOP (16-bit timers; 8-bit ones ignore it: 255). */
void hermes_pwm_hw_timer(int timer, int bits, uint8_t cs, uint16_t top);
/* Connects (on) or disconnects the pin from its timer output. */
void hermes_pwm_hw_connect(int pin, int timer, int ch, int on);
/* Compare value for a duty (ocr already scaled to TOP). */
void hermes_pwm_hw_compare(int pin, int timer, int ch, uint16_t ocr, uint8_t duty);

#endif
