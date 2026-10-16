/* PWM driver, AVR implementation (megaAVR: ATmega328P, ATmega2560...).
 * See hermes_drv_pwm.h.
 *
 * Phase-correct PWM, non-inverting (COMnx1):
 *   Timer0, Timer2 (8 bits): mode 1, TOP = 0xFF (Timer0 only when its role
 *     is pwm: it's the system timer by default, hermes_drv_timers.h);
 *   Timer1, 3, 4, 5 (16 bits): mode 10, TOP = ICRn -- the frequency can
 *     be chosen without losing a channel.
 * In phase-correct mode a compare value of 0 keeps the pin LOW and one of
 * TOP keeps it HIGH: duty 0 and 255 are exact, with no glitch.
 * The 16-bit registers (ICRn, OCRnx) are written with interrupts off: they
 * go through the timer's shared TEMP byte. */
#include "hermes_drv_pwm.h"
#include <avr/io.h>

typedef struct {
    volatile uint8_t *tccra, *tccrb;
    volatile uint16_t *icr;
    volatile uint16_t *ocr[3]; /* A, B, C (C only on the ATmega2560) */
} timer16_t;

static int timer16(int timer, timer16_t *t) {
    switch (timer) {
#ifdef TCCR1A
    case 1:
        *t = (timer16_t){ &TCCR1A, &TCCR1B, &ICR1, { &OCR1A, &OCR1B,
#ifdef OCR1C
                                                     &OCR1C
#else
                                                     0
#endif
                                                   } };
        return 1;
#endif
#ifdef TCCR3A
    case 3: *t = (timer16_t){ &TCCR3A, &TCCR3B, &ICR3, { &OCR3A, &OCR3B, &OCR3C } }; return 1;
#endif
#ifdef TCCR4A
    case 4: *t = (timer16_t){ &TCCR4A, &TCCR4B, &ICR4, { &OCR4A, &OCR4B, &OCR4C } }; return 1;
#endif
#ifdef TCCR5A
    case 5: *t = (timer16_t){ &TCCR5A, &TCCR5B, &ICR5, { &OCR5A, &OCR5B, &OCR5C } }; return 1;
#endif
    default: return 0;
    }
}

/* COMnx1: bit 7 (A), 5 (B), 3 (C) of TCCRnA -- the same on every timer */
#define COM_BIT(ch) (uint8_t)(1u << (7 - 2 * (ch)))

static void write16(volatile uint16_t *reg, uint16_t v) {
    uint8_t sreg = SREG;
    __asm__ __volatile__("cli" ::: "memory");
    *reg = v;
    SREG = sreg;
}

void hermes_pwm_hw_timer(int timer, int bits, uint8_t cs, uint16_t top) {
    (void)bits;
    if (timer == 0) {
        TCCR0A = (uint8_t)((TCCR0A & 0xF0) | (1 << WGM00)); /* keep COM bits */
        TCCR0B = cs;
        return;
    }
    if (timer == 2) {
        TCCR2A = (uint8_t)((TCCR2A & 0xF0) | (1 << WGM20)); /* keep COM bits */
        TCCR2B = cs;
        return;
    }
    timer16_t t;
    if (!timer16(timer, &t)) return;
    write16(t.icr, top);
    *t.tccra = (uint8_t)((*t.tccra & 0xFC) | (1 << 1)); /* WGMn1 */
    *t.tccrb = (uint8_t)((1 << 4) | cs);                 /* WGMn3 + clock */
}

void hermes_pwm_hw_connect(int pin, int timer, int ch, int on) {
    (void)pin;
    volatile uint8_t *tccra;
    if (timer == 0) {
        tccra = &TCCR0A;
    } else if (timer == 2) {
        tccra = &TCCR2A;
    } else {
        timer16_t t;
        if (!timer16(timer, &t)) return;
        tccra = t.tccra;
    }
    uint8_t sreg = SREG;
    __asm__ __volatile__("cli" ::: "memory");
    if (on) *tccra |= COM_BIT(ch);
    else *tccra &= (uint8_t)~COM_BIT(ch);
    SREG = sreg;
}

void hermes_pwm_hw_compare(int pin, int timer, int ch, uint16_t ocr, uint8_t duty) {
    (void)pin; (void)duty;
    if (timer == 0) {
        if (ch == 0) OCR0A = (uint8_t)ocr;
        else OCR0B = (uint8_t)ocr;
        return;
    }
    if (timer == 2) {
        if (ch == 0) OCR2A = (uint8_t)ocr;
        else OCR2B = (uint8_t)ocr;
        return;
    }
    timer16_t t;
    if (timer16(timer, &t) && t.ocr[ch]) write16(t.ocr[ch], ocr);
}
