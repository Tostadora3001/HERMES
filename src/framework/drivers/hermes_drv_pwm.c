/* PWM driver, common part (every target): the board's PWM pins, the
 * PWM state of each timer (whose role, in hermes_drv_timers, says whether
 * PWM may use it), and the duty/frequency arithmetic -- shared by the AVR
 * implementation and the host mock, so freq() gives the same answer on
 * both. See hermes_drv_pwm.h. */
#include "hermes_drv_pwm.h"
#include "hermes_drv_gpio.h"
#include "hermes_drv_timers.h"

/* The mock uses the clock of the boards it imitates (Uno, Mega). */
#ifdef F_CPU
#define HERMES_PWM_CLOCK ((uint32_t)F_CPU)
#else
#define HERMES_PWM_CLOCK 16000000UL
#endif

enum { HERMES_PWM_CH_A = 0, HERMES_PWM_CH_B = 1, HERMES_PWM_CH_C = 2 };

/* The board's table as code (no table in RAM): */
int hermes_pwm_index(int pin) {
    int i = 0;
#define HERMES_PWM_IDX_(p, timer, ch, bits) if (pin == (p)) return i; i++;
    HERMES_BOARD_PWM_PINS(HERMES_PWM_IDX_)
#undef HERMES_PWM_IDX_
    (void)pin; (void)i;
    return -1;
}

#define HERMES_PWM_FIELD_(name, expr)                                     \
    static int name(int pin) {                                            \
        HERMES_BOARD_PWM_PINS(expr)                                       \
        (void)pin;                                                        \
        return -1;                                                        \
    }
#define HERMES_PWM_TIMER_(p, timer, ch, bits) if (pin == (p)) return (timer);
#define HERMES_PWM_CHAN_(p, timer, ch, bits) if (pin == (p)) return HERMES_PWM_CH_##ch;
HERMES_PWM_FIELD_(pin_timer, HERMES_PWM_TIMER_)
HERMES_PWM_FIELD_(pin_channel, HERMES_PWM_CHAN_)

int hermes_pwm_timer_of(int pin) { return pin_timer(pin); }

/* --- state ----------------------------------------------------------------- */

typedef struct {
    uint8_t pins;   /* PWM pins started on it */
    uint8_t cs;     /* prescaler index + 1, 0 = never configured (490 Hz when it is) */
    uint16_t top;
} timer_state_t;

#define HERMES_PWM_SLOTS (HERMES_TIMERS_COUNT > 0 ? HERMES_TIMERS_COUNT : 1)
static timer_state_t g_timer[HERMES_PWM_SLOTS];
/* per PWM pin (index in the board's table): started, duty */
static uint8_t g_on[HERMES_PWM_PIN_COUNT > 0 ? HERMES_PWM_PIN_COUNT : 1];
static uint8_t g_duty[HERMES_PWM_PIN_COUNT > 0 ? HERMES_PWM_PIN_COUNT : 1];

/* Prescalers (megaAVR): Timer2 has 1, 8, 32, 64, 128, 256, 1024 (clock
 * select 1-7); every other timer 1, 8, 64, 256, 1024 (clock select 1-5).
 * The k-th prescaler's clock select is k + 1. As code, not tables: a
 * `const` table is copied to RAM on AVR (it cost 36 B of heap -- enough
 * for a pwm() to run out of memory in a two-thread program). */
static uint8_t prescalers(int timer) { return timer == 2 ? 7 : 5; }
static uint8_t default_k(int timer) { return timer == 2 ? 3 : 2; } /* 64 */

static uint16_t divider(int timer, uint8_t k) {
    if (timer == 2) {
        switch (k) {
        case 0: return 1; case 1: return 8; case 2: return 32; case 3: return 64;
        case 4: return 128; case 5: return 256; default: return 1024;
        }
    }
    switch (k) {
    case 0: return 1; case 1: return 8; case 2: return 64; case 3: return 256; default: return 1024;
    }
}

/* phase-correct: f = clock / (2 * N * TOP); 8-bit timers: TOP = 255 */
static long freq_of(int timer, uint8_t k, uint16_t top) {
    uint32_t den = 2UL * divider(timer, k) * (hermes_timers_bits(timer) == 8 ? 255u : top);
    return (long)((HERMES_PWM_CLOCK + den / 2) / den);
}

static uint16_t ocr_of(int timer, uint8_t duty) {
    if (hermes_timers_bits(timer) == 8) return duty;
    return (uint16_t)(((uint32_t)g_timer[timer].top * duty + 127) / 255);
}

/* The timer's PWM mode at its frequency (the default one the first time). */
static void configure(int timer) {
    timer_state_t *ts = &g_timer[timer];
    if (!ts->cs) {
        ts->cs = (uint8_t)(default_k(timer) + 1);
        ts->top = 255;
    }
    hermes_pwm_hw_timer(timer, hermes_timers_bits(timer), ts->cs, ts->top);
}

/* Rewrites every started pin of `timer` (after its TOP changed). */
static void refresh_timer(int timer) {
#define HERMES_PWM_REFRESH_(p, tm, ch, bits)                                        \
    if ((tm) == timer) {                                                            \
        int i_ = hermes_pwm_index(p);                                               \
        if (g_on[i_]) hermes_pwm_hw_compare((p), (tm), HERMES_PWM_CH_##ch,          \
                                            ocr_of((tm), g_duty[i_]), g_duty[i_]);  \
    }
    HERMES_BOARD_PWM_PINS(HERMES_PWM_REFRESH_)
#undef HERMES_PWM_REFRESH_
}

/* --- API ---------------------------------------------------------------------- */

/* May PWM use `timer`? A free one becomes pwm. */
static hermes_pwm_status_t claim(int timer) {
    switch (hermes_timers_role(timer)) {
    case HERMES_TIMER_PWM: return HERMES_PWM_OK;
    case HERMES_TIMER_FREE:
        hermes_timers_set_role(timer, HERMES_TIMER_PWM, HERMES_TIMER_OWNER_NONE);
        return HERMES_PWM_OK;
    default: return HERMES_PWM_ERR_ROLE;
    }
}

hermes_pwm_status_t hermes_pwm_start(int pin) {
    int i = hermes_pwm_index(pin);
    if (i < 0) return HERMES_PWM_ERR_NO_PWM;
    int timer = pin_timer(pin);
    hermes_pwm_status_t st = claim(timer);
    if (st != HERMES_PWM_OK) return st;
    if (g_on[i]) return HERMES_PWM_OK;
    if (g_timer[timer].pins++ == 0) configure(timer);
    g_on[i] = 1;
    g_duty[i] = 0;
    hermes_gpio_mode(pin, HERMES_GPIO_OUTPUT);
    hermes_pwm_hw_compare(pin, timer, pin_channel(pin), 0, 0);
    hermes_pwm_hw_connect(pin, timer, pin_channel(pin), 1);
    return HERMES_PWM_OK;
}

hermes_pwm_status_t hermes_pwm_write(int pin, uint8_t duty) {
    int i = hermes_pwm_index(pin);
    if (i < 0) return HERMES_PWM_ERR_NO_PWM;
    int timer = pin_timer(pin);
    if (hermes_timers_role(timer) != HERMES_TIMER_PWM) return HERMES_PWM_ERR_ROLE;
    if (!g_on[i]) { /* its timer stopped being pwm and is pwm again */
        hermes_pwm_status_t st = hermes_pwm_start(pin);
        if (st != HERMES_PWM_OK) return st;
    }
    g_duty[i] = duty;
    hermes_pwm_hw_compare(pin, timer, pin_channel(pin), ocr_of(timer, duty), duty);
    return HERMES_PWM_OK;
}

long hermes_pwm_timer_freq(int timer, long hz) {
    if (!hermes_timers_exists(timer) || hz < 1) return 0;
    if (hermes_timers_role(timer) != HERMES_TIMER_PWM) return 0;
    uint8_t best = 0;
    uint16_t top = 255;
    if (hermes_timers_bits(timer) == 8) {
        /* only the prescaler: the nearest frequency */
        long best_err = -1;
        for (uint8_t k = 0; k < prescalers(timer); k++) {
            long f = freq_of(timer, k, 255), err = f > hz ? f - hz : hz - f;
            if (best_err < 0 || err < best_err) { best_err = err; best = k; }
        }
    } else {
        /* the smallest prescaler whose TOP fits: the finest duty steps */
        uint8_t n = prescalers(timer), k = 0;
        for (; k < n; k++) {
            uint32_t den = 2UL * divider(timer, k) * (uint32_t)hz;
            uint32_t t = (HERMES_PWM_CLOCK + den / 2) / den;
            if (t <= 65535u) { top = (uint16_t)t; break; }
        }
        if (k == n) { k = (uint8_t)(n - 1); top = 65535u; } /* below ~0.12 Hz: the slowest */
        if (top < 3) return 0;                               /* too fast for a usable duty */
        best = k;
    }
    g_timer[timer].cs = (uint8_t)(best + 1);
    g_timer[timer].top = top;
    hermes_pwm_hw_timer(timer, hermes_timers_bits(timer), g_timer[timer].cs, top);
    refresh_timer(timer);
    return freq_of(timer, best, top);
}

long hermes_pwm_timer_freq_now(int timer) {
    if (!hermes_timers_exists(timer)) return 0;
    timer_state_t *ts = &g_timer[timer];
    if (!ts->cs) return freq_of(timer, default_k(timer), 255);
    return freq_of(timer, (uint8_t)(ts->cs - 1), ts->top);
}

int hermes_pwm_timer_pins(int timer) {
    return hermes_timers_exists(timer) ? g_timer[timer].pins : 0;
}

long hermes_pwm_freq(int pin, long hz) {
    int i = hermes_pwm_index(pin);
    return i < 0 ? 0 : hermes_pwm_timer_freq(pin_timer(pin), hz);
}

long hermes_pwm_freq_now(int pin) {
    int i = hermes_pwm_index(pin);
    return i < 0 ? 0 : hermes_pwm_timer_freq_now(pin_timer(pin));
}

static void stop_index(int pin, int timer, int ch, int i) {
    hermes_pwm_hw_connect(pin, timer, ch, 0);
    hermes_gpio_write(pin, 0);
    g_on[i] = 0;
    g_duty[i] = 0;
    g_timer[timer].pins--;
}

void hermes_pwm_stop(int pin) {
    int i = hermes_pwm_index(pin);
    if (i < 0 || !g_on[i]) return;
    stop_index(pin, pin_timer(pin), pin_channel(pin), i);
}

int hermes_pwm_duty_now(int pin) {
    int i = hermes_pwm_index(pin);
    return i < 0 || !g_on[i] ? 0 : g_duty[i];
}

#ifdef HERMES_TIMERS /* roles change only with import hermes_timers */
/* hermes_drv_timers: `timer` is no longer pwm -- every pin it drives goes
 * LOW and off (a write() on them is an error until it's pwm again). */
void hermes_pwm_timer_lost(int timer) {
#define HERMES_PWM_LOST_(p, tm, ch, bits)                                    \
    if ((tm) == timer) {                                                     \
        int i_ = hermes_pwm_index(p);                                        \
        if (g_on[i_]) stop_index((p), (tm), HERMES_PWM_CH_##ch, i_);         \
    }
    HERMES_BOARD_PWM_PINS(HERMES_PWM_LOST_)
#undef HERMES_PWM_LOST_
    g_timer[timer].pins = 0;
}

/* Every started pin of `timer` to duty 0 (the timer device's abort). */
void hermes_pwm_timer_zero(int timer) {
#define HERMES_PWM_ZERO_(p, tm, ch, bits)                                    \
    if ((tm) == timer && g_on[hermes_pwm_index(p)]) hermes_pwm_write((p), 0);
    HERMES_BOARD_PWM_PINS(HERMES_PWM_ZERO_)
#undef HERMES_PWM_ZERO_
}
#endif
