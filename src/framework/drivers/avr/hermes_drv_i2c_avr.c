/* I2C driver, AVR implementation: the TWI (ATmega328P, ATmega2560). See
 * hermes_drv_i2c.h. Master by polling TWINT; slave by the TWI interrupt. */
#include "hermes_drv_i2c.h"
#include "hermes_drv_gpio.h" /* the pull-ups of SDA/SCL */
#include "hermes_board.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/atomic.h>

/* (weak: the driver doesn't need the thread runtime; with it, no other
 * thread runs in the middle of a transaction) */
extern void hermes_preempt_off(void) __attribute__((weak));
extern void hermes_preempt_on(void) __attribute__((weak));

#define TW_STATUS (TWSR & 0xF8)

void hermes_i2c_lock(void) {
    if (hermes_preempt_off) hermes_preempt_off();
}

void hermes_i2c_unlock(void) {
    if (hermes_preempt_on) hermes_preempt_on();
}

static void pins(hermes_gpio_mode_t mode) {
    hermes_gpio_mode(HERMES_BOARD_I2C_SDA_PIN, mode);
    hermes_gpio_mode(HERMES_BOARD_I2C_SCL_PIN, mode);
}

/* TWINT, or a timeout: then the TWI is reset (it lets go of the bus) */
static int wait_twint(void) {
    uint16_t n = HERMES_I2C_TIMEOUT_LOOPS;
    while (!(TWCR & _BV(TWINT))) {
        if (--n == 0) {
            TWCR = 0;
            TWCR = _BV(TWEN);
            return 0;
        }
    }
    return 1;
}

/* The status after an operation: one of the codes it can give (a, b, c,
 * d), or 0x38 (arbitration lost) / 0x00 (bus error). On a real TWI the
 * first read already is one. simavr 1.6 sets TWINT some cycles before
 * TWSR in a read (measured: ~60 cycles; the old status meanwhile), so it
 * reads again until it is -- a bounded wait (scripts/simavr_bench). */
static uint8_t tw_result(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint8_t st = TW_STATUS;
    for (uint8_t n = 255; n && st != a && st != b && st != c && st != d && st != 0x38 && st != 0x00; n--)
        st = TW_STATUS;
    return st;
}

/* --- master ------------------------------------------------------------------ */

uint32_t hermes_i2c_master_init(uint32_t hz) {
    uint8_t br, ps;
    uint32_t real = hermes_i2c_divider(hz, &br, &ps);
    TWCR = 0;
    pins(HERMES_GPIO_INPUT_PULLUP);
    TWSR = ps; /* TWPS1:0 */
    TWBR = br;
    TWCR = _BV(TWEN);
    return real;
}

void hermes_i2c_disable(void) {
    TWCR = 0;
    pins(HERMES_GPIO_INPUT);
}

int hermes_i2c_start(uint8_t addr, int read) {
    TWCR = _BV(TWINT) | _BV(TWSTA) | _BV(TWEN);
    if (!wait_twint()) return HERMES_I2C_TIMEOUT;
    uint8_t st = tw_result(0x08, 0x10, 0x08, 0x10); /* START / repeated START */
    if (st != 0x08 && st != 0x10) return HERMES_I2C_BUS_ERROR;
    TWDR = (uint8_t)((addr << 1) | (read ? 1 : 0));
    TWCR = _BV(TWINT) | _BV(TWEN);
    if (!wait_twint()) return HERMES_I2C_TIMEOUT;
    /* SLA+R / SLA+W with ACK (0x40 / 0x18) or NACK (0x48 / 0x20). After
     * SLA+W simavr 1.6 always reports the data codes instead (0x28 /
     * 0x30), which a real TWI never gives there: accepted too, so the
     * driver can be tested on it (scripts/simavr_bench). */
    st = read ? tw_result(0x40, 0x48, 0x40, 0x48) : tw_result(0x18, 0x20, 0x28, 0x30);
    if (st == (read ? 0x40 : 0x18) || (!read && st == 0x28)) return HERMES_I2C_OK;
    if (st == (read ? 0x48 : 0x20) || (!read && st == 0x30)) return HERMES_I2C_NACK_ADDR;
    return HERMES_I2C_BUS_ERROR;                                 /* arbitration lost (0x38)... */
}

int hermes_i2c_send(uint8_t b) {
    TWDR = b;
    TWCR = _BV(TWINT) | _BV(TWEN);
    if (!wait_twint()) return HERMES_I2C_TIMEOUT;
    uint8_t st = tw_result(0x28, 0x30, 0x28, 0x30);
    if (st == 0x28) return HERMES_I2C_OK;
    if (st == 0x30) return HERMES_I2C_NACK_DATA;
    return HERMES_I2C_BUS_ERROR;
}

int hermes_i2c_recv(int ack, uint8_t *b) {
    TWCR = _BV(TWINT) | _BV(TWEN) | (ack ? _BV(TWEA) : 0);
    if (!wait_twint()) return HERMES_I2C_TIMEOUT;
    uint8_t st = tw_result(0x50, 0x58, 0x50, 0x58); /* data received, ACK / NACK sent */
    *b = TWDR;
    if (st == 0x50 || st == 0x58) return HERMES_I2C_OK;
    return HERMES_I2C_BUS_ERROR;
}

void hermes_i2c_stop(void) {
    TWCR = _BV(TWINT) | _BV(TWSTO) | _BV(TWEN);
    uint16_t n = HERMES_I2C_TIMEOUT_LOOPS;
    while ((TWCR & _BV(TWSTO)) && --n) {}
}

/* --- slave --------------------------------------------------------------------
 * Only with -DHERMES_I2C_SLAVE (hepyc passes it when the program calls
 * i2c_slave): the interrupt is linked as soon as it exists, and with it
 * its buffers -- 64 B of RAM a master-only program shouldn't pay. */
#ifdef HERMES_I2C_SLAVE

static volatile uint8_t g_rx[HERMES_I2C_SLAVE_RX], g_tx[HERMES_I2C_SLAVE_TX];
static volatile uint8_t g_rx_head, g_rx_tail, g_tx_head, g_tx_tail;

#define SLAVE_ACK (_BV(TWINT) | _BV(TWEA) | _BV(TWEN) | _BV(TWIE))

void hermes_i2c_slave_init(uint8_t addr) {
    TWCR = 0;
    g_rx_head = g_rx_tail = g_tx_head = g_tx_tail = 0;
    pins(HERMES_GPIO_INPUT_PULLUP);
    TWAR = (uint8_t)(addr << 1); /* no general call */
    TWCR = SLAVE_ACK;
    sei();
}

ISR(TWI_vect) {
    switch (TW_STATUS) {
        case 0x80: case 0x90: { /* data from the master, ACK sent */
            uint8_t next = (uint8_t)((g_rx_head + 1) & (HERMES_I2C_SLAVE_RX - 1));
            if (next != g_rx_tail) { g_rx[g_rx_head] = TWDR; g_rx_head = next; } /* full: dropped */
            break;
        }
        case 0xA8: case 0xB0: case 0xB8: /* the master reads: next queued byte */
            if (g_tx_tail != g_tx_head) {
                TWDR = g_tx[g_tx_tail];
                g_tx_tail = (uint8_t)((g_tx_tail + 1) & (HERMES_I2C_SLAVE_TX - 1));
            } else {
                TWDR = 0xFF;
            }
            break;
        case 0x00: /* bus error: release the lines */
            TWCR = SLAVE_ACK | _BV(TWSTO);
            return;
        default: /* own SLA+W (0x60...), STOP (0xA0), last byte NACKed (0xC0, 0xC8)... */
            break;
    }
    TWCR = SLAVE_ACK;
}

int hermes_i2c_slave_available(void) {
    uint8_t n;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { n = (uint8_t)((g_rx_head - g_rx_tail) & (HERMES_I2C_SLAVE_RX - 1)); }
    return n;
}

int hermes_i2c_slave_read(void) {
    int b = -1;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (g_rx_tail != g_rx_head) {
            b = g_rx[g_rx_tail];
            g_rx_tail = (uint8_t)((g_rx_tail + 1) & (HERMES_I2C_SLAVE_RX - 1));
        }
    }
    return b;
}

int hermes_i2c_slave_reply(uint8_t b) {
    int ok = 0;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        uint8_t next = (uint8_t)((g_tx_head + 1) & (HERMES_I2C_SLAVE_TX - 1));
        if (next != g_tx_tail) { g_tx[g_tx_head] = b; g_tx_head = next; ok = 1; }
    }
    return ok;
}

int hermes_i2c_slave_replies(void) {
    uint8_t n;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { n = (uint8_t)((g_tx_head - g_tx_tail) & (HERMES_I2C_SLAVE_TX - 1)); }
    return n;
}

#endif /* HERMES_I2C_SLAVE */
