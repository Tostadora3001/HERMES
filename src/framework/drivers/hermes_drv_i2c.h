#ifndef HERMES_DRV_I2C_H
#define HERMES_DRV_I2C_H

/* Core driver: the I2C bus (AVR: the TWI; SDA/SCL = the board's
 * HERMES_BOARD_I2C_SDA_PIN / _SCL_PIN: Uno A4/A5, Mega D20/D21), as master
 * or as slave -- one or the other at a time.
 *
 * Common interface; one implementation per platform:
 *   hermes_drv_i2c.c               common part: transactions (register
 *                                  reads/writes...) over the primitives,
 *                                  the clock divider arithmetic, error names
 *   avr/hermes_drv_i2c_avr.c       TWI: master by polling, slave by its
 *                                  interrupt
 *   native/hermes_drv_i2c_native.c a mock: virtual chips on the bus (an
 *                                  EEPROM at 0x50 by default) and, for the
 *                                  slave, a virtual master driven from C
 * No ESP32-C3 implementation yet: hepyc reports it (decided with the user).
 *
 * Master: every primitive waits for the hardware without yielding (a byte
 * takes ~90 us at 100 kHz) and gives up after HERMES_I2C_TIMEOUT_LOOPS
 * polls (~15 ms on a 16 MHz AVR): a stuck bus is an error, never a hang.
 * Errors are returned (decided with the user: a missing chip is normal),
 * never fatal. A transaction runs between hermes_i2c_lock() and
 * hermes_i2c_unlock(): bare metal = no preemption meanwhile (one core);
 * host = a mutex. The internal pull-ups of SDA/SCL are switched on, as
 * Arduino's Wire does -- a real bus still wants external ones (4.7 kOhm).
 *
 * Slave: an interrupt answers the master by itself. What the master writes
 * goes to a ring of HERMES_I2C_SLAVE_RX bytes (dropped when full); what
 * it reads comes from a queue of HERMES_I2C_SLAVE_TX bytes the program
 * fills in advance (hermes_i2c_slave_reply); with the queue empty the
 * master gets 0xFF. */

#include <stdint.h>

#ifndef HERMES_I2C_TIMEOUT_LOOPS
#define HERMES_I2C_TIMEOUT_LOOPS 60000u
#endif
#ifndef HERMES_I2C_SLAVE_RX
#define HERMES_I2C_SLAVE_RX 32 /* power of two */
#endif
#ifndef HERMES_I2C_SLAVE_TX
#define HERMES_I2C_SLAVE_TX 32 /* power of two */
#endif

#define HERMES_I2C_MIN_HZ 1000L
#define HERMES_I2C_MAX_HZ 400000L

/* Result of every master operation. */
enum {
    HERMES_I2C_OK = 0,
    HERMES_I2C_NACK_ADDR, /* nobody answered at that address */
    HERMES_I2C_NACK_DATA, /* the chip refused a byte */
    HERMES_I2C_TIMEOUT,   /* the bus didn't move (held low, no pull-ups...) */
    HERMES_I2C_BUS_ERROR, /* lost arbitration, or a bus error */
    HERMES_I2C_OFF,       /* the bus was aborted */
};

/* --- implemented by each platform ---------------------------------------- */

/* Master on, at the closest frequency to `hz` it can do; returns it. */
uint32_t hermes_i2c_master_init(uint32_t hz);
/* TWI off (master or slave), SDA/SCL back to plain inputs. */
void hermes_i2c_disable(void);
/* (Repeated) START, then the address with R/W. */
int hermes_i2c_start(uint8_t addr, int read);
int hermes_i2c_send(uint8_t b);
/* One byte; ack = more will be read (the last one is NACKed). */
int hermes_i2c_recv(int ack, uint8_t *b);
void hermes_i2c_stop(void);
void hermes_i2c_lock(void);
void hermes_i2c_unlock(void);

/* Slave on at `addr` (7 bits); buffers emptied. */
void hermes_i2c_slave_init(uint8_t addr);
int hermes_i2c_slave_available(void);   /* bytes the master wrote, waiting */
int hermes_i2c_slave_read(void);        /* next one, or -1 if none */
int hermes_i2c_slave_reply(uint8_t b);  /* queue it for the master; 0 = queue full */
int hermes_i2c_slave_replies(void);     /* bytes still queued */

/* --- common part ------------------------------------------------------------ */

/* AVR's TWI arithmetic (SCL = F_CPU / (16 + 2 * TWBR * 4^TWPS)), shared
 * with the mock so both report the same frequency: the closest it can do
 * to `hz`, not above it. */
uint32_t hermes_i2c_divider(uint32_t hz, uint8_t *twbr, uint8_t *twps);
const char *hermes_i2c_error_name(int err); /* "ok", "nack_addr"... (for print) */

/* Whole transactions (START ... STOP, under the lock). Each returns an
 * HERMES_I2C_* code; reads put the value in *out. reg16 = two bytes, high
 * byte first (what almost every sensor does). */
int hermes_i2c_probe(uint8_t addr);
int hermes_i2c_write_byte(uint8_t addr, uint8_t b);
int hermes_i2c_read_byte(uint8_t addr, uint8_t *out);
int hermes_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t v);
int hermes_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *out);
int hermes_i2c_write_reg16(uint8_t addr, uint8_t reg, uint16_t v);
int hermes_i2c_read_reg16(uint8_t addr, uint8_t reg, uint16_t *out);

#if !defined(__AVR__) && !defined(__riscv)
/* Host mock only (insert_C in a test): a virtual chip at `addr` with
 * `size` (1-256) registers behind an auto-incrementing pointer -- the
 * first byte written sets it, then each byte written or read moves it on
 * (like a 24C02 EEPROM, or most sensors). An EEPROM of 256 bytes at 0x50
 * is there from the start. remove: it stops answering. */
int hermes_i2c_mock_add(int addr, int size);
void hermes_i2c_mock_remove(int addr);
/* The program is the slave: a virtual master writes `b` to it / reads one
 * byte from it. -1 if no slave answers at `addr`. */
int hermes_i2c_mock_master_send(int addr, int b);
int hermes_i2c_mock_master_read(int addr);
#endif

#endif
