/* GPIO driver, AVR implementation (megaAVR: ATmega328P, ATmega2560...).
 * See hermes_drv_gpio.h.
 *
 * The board's pin map (HERMES_BOARD_PINS) becomes a table in flash, one
 * byte per pin: port number << 3 | bit. In flash (PROGMEM) because on AVR
 * a plain `const` table is also copied to RAM: 70 bytes on a Mega.
 *
 * Every megaAVR port has its three registers at consecutive addresses:
 * PINx, DDRx = PINx + 1, PORTx = PINx + 2 (ports A-G in I/O space, H-L
 * of the ATmega2560 in extended I/O). Only the address of PINx is looked
 * up; a port the chip doesn't have is a compile error in the board's
 * table, never a wrong address at run time. */
#include "hermes_drv_gpio.h"
#include <avr/io.h>
#include <avr/pgmspace.h>

enum {
    HERMES_PORT_A = 1, HERMES_PORT_B, HERMES_PORT_C, HERMES_PORT_D, HERMES_PORT_E,
    HERMES_PORT_F, HERMES_PORT_G, HERMES_PORT_H, HERMES_PORT_J, HERMES_PORT_K,
    HERMES_PORT_L,
};

/* `port` must be a port this chip has: the check below turns X(Q, 0) on a
 * chip without PINQ into "PINQ undeclared" at compile time. */
#define HERMES_PIN_ENTRY_(port, bit) \
    (uint8_t)((HERMES_PORT_##port << 3) | ((bit) & 7)),
static const uint8_t PIN_MAP[HERMES_GPIO_PIN_COUNT] PROGMEM = {
    HERMES_BOARD_PINS(HERMES_PIN_ENTRY_)
};
#define HERMES_PIN_CHECK_(port, bit) (void)&PIN##port;
static inline void pin_map_check(void) { HERMES_BOARD_PINS(HERMES_PIN_CHECK_) }

/* Data-space address of PINx per port number, 0 if this chip has no such
 * port. In flash: a `switch` here became a table of pointers that avr-gcc
 * keeps in RAM (measured: +6 B of .data on the Uno). */
static const uint16_t PORT_PIN_ADDR[HERMES_PORT_L + 1] PROGMEM = {
#ifdef PINA
    [HERMES_PORT_A] = _SFR_MEM_ADDR(PINA),
#endif
#ifdef PINB
    [HERMES_PORT_B] = _SFR_MEM_ADDR(PINB),
#endif
#ifdef PINC
    [HERMES_PORT_C] = _SFR_MEM_ADDR(PINC),
#endif
#ifdef PIND
    [HERMES_PORT_D] = _SFR_MEM_ADDR(PIND),
#endif
#ifdef PINE
    [HERMES_PORT_E] = _SFR_MEM_ADDR(PINE),
#endif
#ifdef PINF
    [HERMES_PORT_F] = _SFR_MEM_ADDR(PINF),
#endif
#ifdef PING
    [HERMES_PORT_G] = _SFR_MEM_ADDR(PING),
#endif
#ifdef PINH
    [HERMES_PORT_H] = _SFR_MEM_ADDR(PINH),
#endif
#ifdef PINJ
    [HERMES_PORT_J] = _SFR_MEM_ADDR(PINJ),
#endif
#ifdef PINK
    [HERMES_PORT_K] = _SFR_MEM_ADDR(PINK),
#endif
#ifdef PINL
    [HERMES_PORT_L] = _SFR_MEM_ADDR(PINL),
#endif
};

static volatile uint8_t *port_pin_reg(uint8_t port) {
    if (port > HERMES_PORT_L) return 0;
    return (volatile uint8_t *)pgm_read_word(&PORT_PIN_ADDR[port]);
}

typedef struct {
    volatile uint8_t *pin;  /* PINx; DDRx = pin + 1, PORTx = pin + 2 */
    uint8_t mask;
} pin_regs_t;

static int pin_regs(int pin, pin_regs_t *r) {
    (void)pin_map_check;
    if (!hermes_gpio_valid(pin)) return 0;
    uint8_t e = pgm_read_byte(&PIN_MAP[pin]);
    r->pin = port_pin_reg(e >> 3);
    r->mask = (uint8_t)(1u << (e & 7));
    return r->pin != 0;
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
        reg_update(r.pin + 1, r.mask, true);
    } else {
        reg_update(r.pin + 1, r.mask, false);
        reg_update(r.pin + 2, r.mask, mode == HERMES_GPIO_INPUT_PULLUP);
    }
    return 1;
}

void hermes_gpio_write(int pin, bool high) {
    pin_regs_t r;
    if (pin_regs(pin, &r)) reg_update(r.pin + 2, r.mask, high);
}

bool hermes_gpio_read(int pin) {
    pin_regs_t r;
    if (!pin_regs(pin, &r)) return false;
    return (*r.pin & r.mask) != 0;
}
