#include "hermes_drv_gpio.h"

int hermes_gpio_valid(int pin) {
    return pin >= 0 && pin < HERMES_GPIO_PIN_COUNT;
}

#if defined(__AVR__)

#include <avr/io.h>

typedef struct {
    volatile uint8_t *ddr;
    volatile uint8_t *port;
    volatile uint8_t *pin;
    uint8_t mask;
} pin_regs_t;

static int pin_regs(int pin, pin_regs_t *r) {
    if (pin >= 0 && pin <= 7) {
        r->ddr = &DDRD; r->port = &PORTD; r->pin = &PIND; r->mask = (uint8_t)(1u << pin);
    } else if (pin >= 8 && pin <= 13) {
        r->ddr = &DDRB; r->port = &PORTB; r->pin = &PINB; r->mask = (uint8_t)(1u << (pin - 8));
    } else if (pin >= 14 && pin <= 19) {
        r->ddr = &DDRC; r->port = &PORTC; r->pin = &PINC; r->mask = (uint8_t)(1u << (pin - 14));
    } else {
        return 0;
    }
    return 1;
}

/* DDRx/PORTx updates are read-modify-write on a runtime address (lds/ori/
 * sts), not a single sbi -- an ISR touching the same port in between would
 * lose its change. Interrupts off for the few cycles it takes. */
static void reg_update(volatile uint8_t *reg, uint8_t mask, bool set) {
    uint8_t sreg = SREG;
    __asm__ __volatile__("cli" ::: "memory");
    if (set) *reg |= mask;
    else *reg &= (uint8_t)~mask;
    SREG = sreg;
}

int hermes_gpio_mode(int pin, hermes_gpio_mode_t mode) {
    pin_regs_t r;
    if (!pin_regs(pin, &r)) return 0;
    if (mode == HERMES_GPIO_OUTPUT) {
        reg_update(r.ddr, r.mask, true);
    } else {
        reg_update(r.ddr, r.mask, false);
        reg_update(r.port, r.mask, mode == HERMES_GPIO_INPUT_PULLUP);
    }
    return 1;
}

void hermes_gpio_write(int pin, bool high) {
    pin_regs_t r;
    if (pin_regs(pin, &r)) reg_update(r.port, r.mask, high);
}

bool hermes_gpio_read(int pin) {
    pin_regs_t r;
    if (!pin_regs(pin, &r)) return false;
    return (*r.pin & r.mask) != 0;
}

#else /* host mock */

#include <stdio.h>

static hermes_gpio_mode_t g_mode[HERMES_GPIO_PIN_COUNT];
static bool g_level[HERMES_GPIO_PIN_COUNT];

int hermes_gpio_mode(int pin, hermes_gpio_mode_t mode) {
    if (!hermes_gpio_valid(pin)) return 0;
    g_mode[pin] = mode;
    if (mode == HERMES_GPIO_INPUT_PULLUP) g_level[pin] = true; /* pulled up until driven low */
    return 1;
}

void hermes_gpio_write(int pin, bool high) {
    if (!hermes_gpio_valid(pin)) return;
    g_level[pin] = high;
    printf("[gpio mock] pin %d -> %s\n", pin, high ? "HIGH" : "LOW");
}

bool hermes_gpio_read(int pin) {
    return hermes_gpio_valid(pin) ? g_level[pin] : false;
}

void hermes_gpio_mock_set_input(int pin, bool high) {
    if (hermes_gpio_valid(pin)) g_level[pin] = high;
}

#endif
