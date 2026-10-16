#ifndef HERMES_I2C_H
#define HERMES_I2C_H

/* `import hermes_i2c`: the I2C bus as built-in (core) devices -- master or
 * slave, one or the other. hepyc links the I2C driver
 * (src/framework/drivers/hermes_drv_i2c.c) and the runtime automatically.
 * See docs/raw/hermes_i2c.en.md.
 *
 * Master: the bus, and the chips on it.
 *   device bus = i2c(100000)        clock in Hz, 1000-400000 (the closest
 *                                   the hardware does, never above)
 *   bus.frequency()                 the clock it really runs at
 *   bus.probe(0x68)                 True if a chip answers there
 *   bus.write(addr, b)              one byte                    } True / False
 *   bus.write_reg(addr, reg, v)     register = v                }
 *   bus.write_reg16(addr, reg, v)   two bytes, high byte first  }
 *   bus.read(addr)                  one byte                    } the value,
 *   bus.read_reg(addr, reg)         a register                  } or -1 on
 *   bus.read_reg16(addr, reg)       two bytes, high byte first  } error
 *   bus.error()                     why the last operation failed: "ok",
 *                                   "nack_addr", "nack_data", "timeout",
 *                                   "bus_error", "off"
 *   device imu = bus.at(0x68)       a chip of the bus as a device of its own:
 *   imu.probe(), imu.write(b), imu.write_reg(reg, v), imu.read_reg(reg)...
 *                                   the same without the address;
 *                                   imu.address(), imu.error() (its own)
 *   bus.abort() / imu.abort()       mandatory abort: the bus off (its
 *                                   operations then fail with "off") /
 *                                   nothing (a chip drives nothing itself)
 *   imu.destroy()                   memory released, the variable NULL
 *   bus.destroy()                   the bus off and released (SDA/SCL plain
 *                                   inputs), after the transfer in progress;
 *                                   a fatal error while chips of the bus
 *                                   (bus.at) still exist
 *   bus.safe_destroy()              the same, destroying its chips first
 *                                   (their variables are left dangling:
 *                                   using one afterwards isn't detected)
 *
 * Slave: the program answers a master. What the master writes waits in a
 * queue; what it reads is queued in advance (decided with the user).
 *   device s = i2c_slave(0x42)      own 7-bit address (0x08-0x77)
 *   s.available()                   bytes the master wrote, waiting
 *   s.read()                        the next one, or -1 if none
 *   s.reply(b)                      queue a byte for the master's next read
 *                                   (False if the queue is full); with
 *                                   nothing queued the master gets 0xFF
 *   s.replies()                     bytes still queued
 *   s.address(), s.abort() (stops answering), s.destroy()
 *
 * Errors of a transfer are returned (decided with the user), never fatal.
 * Fatal (with a message): a frequency or address out of range, two chips
 * at the same address, i2c() and i2c_slave() together or with SDA/SCL
 * held by another device (hardware conflicts, hermes_device.h), any
 * method after destroy(). Transfers don't yield and never hang (timeout). */

#include "hermes_device.h"
#include "hermes_drv_i2c.h"
#include "hermes_sched_internal.h" /* hermes_runtime_alloc, hermes_lock */
#include "hermes_diag.h"
#include "hermes_board.h"
#include <stdbool.h>

#ifndef HERMES_BOARD_I2C_SDA_PIN
#define HERMES_BOARD_I2C_SDA_PIN -1
#endif
#ifndef HERMES_BOARD_I2C_SCL_PIN
#define HERMES_BOARD_I2C_SCL_PIN -1
#endif

/* ============================================================================
 * master: the bus
 * ========================================================================== */

typedef struct {
    hermes_device_t device;
    long hz;                /* the real clock */
    signed char err;        /* last operation's result */
    unsigned char off;      /* aborted */
} hermes_device_i2c_t;

typedef hermes_handle_t hermes_device_i2c_handle_t;
HERMES_DEVICE_KIND(i2c);

typedef struct {
    hermes_device_t device;
    hermes_device_i2c_t *bus;
    unsigned char addr;
    signed char err;
} hermes_device_i2c_dev_t;

typedef hermes_handle_t hermes_device_i2c_dev_handle_t;
HERMES_DEVICE_KIND(i2c_dev);

/* Mandatory abort: the bus off -- nothing more goes out on it. */
static inline void device_i2c_abort(hermes_device_i2c_handle_t h_b) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "abort");
    hermes_i2c_lock(); /* after the transfer in progress */
    hermes_i2c_disable();
    b->off = 1;
    hermes_i2c_unlock();
}

static inline long hermes_i2c_devices_of(hermes_device_i2c_t *b);

/* Mandatory describe: SDA/SCL, memory, clock and its chips. */
static inline void device_i2c_describe(hermes_device_i2c_handle_t h_b, hermes_device_desc_t *o) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "describe");
    o->pins[0] = HERMES_BOARD_I2C_SDA_PIN;
    o->pins[1] = HERMES_BOARD_I2C_SCL_PIN;
    o->bytes = sizeof(hermes_device_i2c_t);
    HERMES_DEVICE_VALUE(o, "hz", b->hz);
    HERMES_DEVICE_VALUE(o, "devices", hermes_i2c_devices_of(b));
    (void)b;
}

/* static via the macro: only the generated .c includes this header. One
 * bus (one TWI on these chips). */
HERMES_DEVICE_CLASS(i2c, hermes_device_i2c_handle_t, 1);

/* --- a chip of the bus (bus.at) --- */

static inline void device_i2c_dev_abort(hermes_device_i2c_dev_handle_t h_d) {
    (void)HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "abort"); /* drives nothing itself */
}

static inline void device_i2c_dev_describe(hermes_device_i2c_dev_handle_t h_d, hermes_device_desc_t *o) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "describe");
    o->bytes = sizeof(hermes_device_i2c_dev_t); /* no pins: the bus holds them */
    HERMES_DEVICE_VALUE(o, "addr", d->addr);
    (void)d;
}

HERMES_DEVICE_CLASS(i2c_dev, hermes_device_i2c_dev_handle_t, 0);

/* Lock held: chips of bus b. */
static inline long hermes_i2c_devices_of(hermes_device_i2c_t *b) {
    long n = 0;
    for (hermes_device_t *it = hermes_device_class_i2c_dev.head; it; it = it->next)
        if (((hermes_device_i2c_dev_t *)it)->bus == b) n++;
    return n;
}

static inline void hermes_i2c_check_addr(long addr, const char *what_P) {
    if (addr >= 0 && addr <= 127) return;
    HERMES_DIAG("hermes_i2c: %" HERMES_PS "(%ld): an I2C address is 7 bits (0-127)\n", what_P, addr);
    (void)what_P;
    hermes_fatal();
}

static inline hermes_device_i2c_handle_t i2c(long hz) {
    if (hz < HERMES_I2C_MIN_HZ || hz > HERMES_I2C_MAX_HZ) {
        HERMES_DIAG("hermes_i2c: i2c(%ld): the clock must be between %ld and %ld Hz\n", hz,
                    HERMES_I2C_MIN_HZ, HERMES_I2C_MAX_HZ);
        hermes_fatal();
    }
    hermes_lock();
    hermes_device_pin_claim(HERMES_BOARD_I2C_SDA_PIN, HERMES_PSTR("i2c"), hz);
    hermes_device_pin_claim(HERMES_BOARD_I2C_SCL_PIN, HERMES_PSTR("i2c"), hz);
    hermes_device_i2c_t *b = hermes_runtime_alloc(sizeof(hermes_device_i2c_t));
    if (!b) {
        hermes_unlock();
        HERMES_DIAG("hermes_i2c: i2c(%ld): out of memory (%u bytes)\n", hz, (unsigned)sizeof(hermes_device_i2c_t));
        hermes_fatal();
    }
    b->err = HERMES_I2C_OK;
    b->off = 0;
    hermes_device_register_or_abort(&b->device, &hermes_device_class_i2c);
    hermes_unlock();
    b->hz = (long)hermes_i2c_master_init((uint32_t)hz);
    return hermes_device_handle(&b->device);
}

/* destroy (both): the bus off, after the transfer in progress */
static inline void hermes_i2c_release(hermes_device_t *d) {
    (void)d;
    hermes_i2c_lock();
    hermes_i2c_disable();
    hermes_i2c_unlock();
}

static inline void device_i2c_destroy(hermes_device_i2c_handle_t h_b) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "destroy");
    hermes_lock();
    long n = hermes_i2c_devices_of(b);
    hermes_unlock();
    if (n) {
        HERMES_DIAG("hermes_i2c: destroy() while %ld devices of the bus (bus.at) exist: destroy them "
                    "first, or use safe_destroy()\n", n);
        hermes_fatal();
    }
    hermes_device_destroy(&b->device, hermes_i2c_release);
}

/* Its chips first (decided with the user: destroyed for good; their
 * variables keep a stale handle -- using one is a fatal error with its
 * message, see "handles" in hermes_device.h), then the bus. */
static inline void device_i2c_safe_destroy(hermes_device_i2c_handle_t h_b) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "safe_destroy");
    for (;;) {
        hermes_device_t *mine = NULL;
        hermes_lock();
        for (hermes_device_t *it = hermes_device_class_i2c_dev.head; it && !mine; it = it->next)
            if (((hermes_device_i2c_dev_t *)it)->bus == b) mine = it;
        hermes_unlock();
        if (!mine) break;
        hermes_device_destroy(mine, 0);
    }
    hermes_device_destroy(&b->device, hermes_i2c_release);
}

/* --- operations, shared by the bus and its chips ---------------------------- */

static inline int hermes_i2c_op_status(hermes_device_i2c_t *b, int st, signed char *err) {
    *err = (signed char)st;
    b->err = (signed char)st;
    return st;
}

#define HERMES_I2C_OFF_CHECK(b, err, failed)                                       \
    do { if ((b)->off) { *(err) = (b)->err = HERMES_I2C_OFF; return (failed); } } while (0)

static inline bool hermes_i2c_do_probe(hermes_device_i2c_t *b, long addr, signed char *err) {
    HERMES_I2C_OFF_CHECK(b, err, false);
    return hermes_i2c_op_status(b, hermes_i2c_probe((uint8_t)addr), err) == HERMES_I2C_OK;
}

static inline bool hermes_i2c_do_write(hermes_device_i2c_t *b, long addr, long v, signed char *err) {
    HERMES_I2C_OFF_CHECK(b, err, false);
    return hermes_i2c_op_status(b, hermes_i2c_write_byte((uint8_t)addr, (uint8_t)v), err) == HERMES_I2C_OK;
}

static inline long hermes_i2c_do_read(hermes_device_i2c_t *b, long addr, signed char *err) {
    HERMES_I2C_OFF_CHECK(b, err, -1);
    uint8_t v;
    return hermes_i2c_op_status(b, hermes_i2c_read_byte((uint8_t)addr, &v), err) == HERMES_I2C_OK ? v : -1;
}

static inline bool hermes_i2c_do_write_reg(hermes_device_i2c_t *b, long addr, long reg, long v, signed char *err) {
    HERMES_I2C_OFF_CHECK(b, err, false);
    return hermes_i2c_op_status(b, hermes_i2c_write_reg((uint8_t)addr, (uint8_t)reg, (uint8_t)v), err)
           == HERMES_I2C_OK;
}

static inline bool hermes_i2c_do_write_reg16(hermes_device_i2c_t *b, long addr, long reg, long v, signed char *err) {
    HERMES_I2C_OFF_CHECK(b, err, false);
    return hermes_i2c_op_status(b, hermes_i2c_write_reg16((uint8_t)addr, (uint8_t)reg, (uint16_t)v), err)
           == HERMES_I2C_OK;
}

static inline long hermes_i2c_do_read_reg(hermes_device_i2c_t *b, long addr, long reg, signed char *err) {
    HERMES_I2C_OFF_CHECK(b, err, -1);
    uint8_t v;
    return hermes_i2c_op_status(b, hermes_i2c_read_reg((uint8_t)addr, (uint8_t)reg, &v), err) == HERMES_I2C_OK
           ? v : -1;
}

static inline long hermes_i2c_do_read_reg16(hermes_device_i2c_t *b, long addr, long reg, signed char *err) {
    HERMES_I2C_OFF_CHECK(b, err, -1);
    uint16_t v;
    return hermes_i2c_op_status(b, hermes_i2c_read_reg16((uint8_t)addr, (uint8_t)reg, &v), err) == HERMES_I2C_OK
           ? (long)v : -1;
}

/* --- the bus's methods ------------------------------------------------------ */

static inline long device_i2c_frequency(hermes_device_i2c_handle_t h_b) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "frequency");
    return b->hz;
}

static inline const char *device_i2c_error(hermes_device_i2c_handle_t h_b) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "error");
    return hermes_i2c_error_name(b->err);
}

static inline bool device_i2c_probe(hermes_device_i2c_handle_t h_b, long addr) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "probe");
    hermes_i2c_check_addr(addr, HERMES_PSTR("probe"));
    return hermes_i2c_do_probe(b, addr, &b->err);
}

static inline bool device_i2c_write(hermes_device_i2c_handle_t h_b, long addr, long v) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "write");
    hermes_i2c_check_addr(addr, HERMES_PSTR("write"));
    return hermes_i2c_do_write(b, addr, v, &b->err);
}

static inline long device_i2c_read(hermes_device_i2c_handle_t h_b, long addr) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "read");
    hermes_i2c_check_addr(addr, HERMES_PSTR("read"));
    return hermes_i2c_do_read(b, addr, &b->err);
}

static inline bool device_i2c_write_reg(hermes_device_i2c_handle_t h_b, long addr, long reg, long v) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "write_reg");
    hermes_i2c_check_addr(addr, HERMES_PSTR("write_reg"));
    return hermes_i2c_do_write_reg(b, addr, reg, v, &b->err);
}

static inline bool device_i2c_write_reg16(hermes_device_i2c_handle_t h_b, long addr, long reg, long v) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "write_reg16");
    hermes_i2c_check_addr(addr, HERMES_PSTR("write_reg16"));
    return hermes_i2c_do_write_reg16(b, addr, reg, v, &b->err);
}

static inline long device_i2c_read_reg(hermes_device_i2c_handle_t h_b, long addr, long reg) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "read_reg");
    hermes_i2c_check_addr(addr, HERMES_PSTR("read_reg"));
    return hermes_i2c_do_read_reg(b, addr, reg, &b->err);
}

static inline long device_i2c_read_reg16(hermes_device_i2c_handle_t h_b, long addr, long reg) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "read_reg16");
    hermes_i2c_check_addr(addr, HERMES_PSTR("read_reg16"));
    return hermes_i2c_do_read_reg16(b, addr, reg, &b->err);
}

/* bus.at(addr): a chip of the bus as a device. Two at the same address is
 * a fatal error (the same chip twice). */
static inline hermes_device_i2c_dev_handle_t device_i2c_at(hermes_device_i2c_handle_t h_b, long addr) {
    hermes_device_i2c_t *b = HERMES_DEVICE_GET(hermes_device_i2c_t, i2c, h_b, "at");
    hermes_i2c_check_addr(addr, HERMES_PSTR("at"));
    hermes_lock();
    for (hermes_device_t *it = hermes_device_class_i2c_dev.head; it; it = it->next) {
        hermes_device_i2c_dev_t *o = (hermes_device_i2c_dev_t *)it;
        if (o->bus == b && o->addr == addr) {
            hermes_unlock();
            HERMES_DIAG("hermes_i2c: at(%ld): the bus already has a device at that address\n", addr);
            hermes_fatal();
        }
    }
    hermes_device_i2c_dev_t *d = hermes_runtime_alloc(sizeof(hermes_device_i2c_dev_t));
    if (!d) {
        hermes_unlock();
        HERMES_DIAG("hermes_i2c: at(%ld): out of memory (%u bytes)\n", addr, (unsigned)sizeof(hermes_device_i2c_dev_t));
        hermes_fatal();
    }
    d->bus = b;
    d->addr = (unsigned char)addr;
    d->err = HERMES_I2C_OK;
    hermes_device_register_or_abort(&d->device, &hermes_device_class_i2c_dev);
    hermes_unlock();
    return hermes_device_handle(&d->device);
}

/* --- a chip's methods (the bus's, with its address) ------------------------- */

static inline void device_i2c_dev_destroy(hermes_device_i2c_dev_handle_t h_d) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "destroy");
    hermes_device_destroy(&d->device, 0);
}

static inline long device_i2c_dev_address(hermes_device_i2c_dev_handle_t h_d) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "address");
    return d->addr;
}

static inline const char *device_i2c_dev_error(hermes_device_i2c_dev_handle_t h_d) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "error");
    return hermes_i2c_error_name(d->err);
}

static inline bool device_i2c_dev_probe(hermes_device_i2c_dev_handle_t h_d) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "probe");
    return hermes_i2c_do_probe(d->bus, d->addr, &d->err);
}

static inline bool device_i2c_dev_write(hermes_device_i2c_dev_handle_t h_d, long v) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "write");
    return hermes_i2c_do_write(d->bus, d->addr, v, &d->err);
}

static inline long device_i2c_dev_read(hermes_device_i2c_dev_handle_t h_d) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "read");
    return hermes_i2c_do_read(d->bus, d->addr, &d->err);
}

static inline bool device_i2c_dev_write_reg(hermes_device_i2c_dev_handle_t h_d, long reg, long v) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "write_reg");
    return hermes_i2c_do_write_reg(d->bus, d->addr, reg, v, &d->err);
}

static inline bool device_i2c_dev_write_reg16(hermes_device_i2c_dev_handle_t h_d, long reg, long v) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "write_reg16");
    return hermes_i2c_do_write_reg16(d->bus, d->addr, reg, v, &d->err);
}

static inline long device_i2c_dev_read_reg(hermes_device_i2c_dev_handle_t h_d, long reg) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "read_reg");
    return hermes_i2c_do_read_reg(d->bus, d->addr, reg, &d->err);
}

static inline long device_i2c_dev_read_reg16(hermes_device_i2c_dev_handle_t h_d, long reg) {
    hermes_device_i2c_dev_t *d = HERMES_DEVICE_GET(hermes_device_i2c_dev_t, i2c_dev, h_d, "read_reg16");
    return hermes_i2c_do_read_reg16(d->bus, d->addr, reg, &d->err);
}

/* ============================================================================
 * slave
 * ========================================================================== */

typedef struct {
    hermes_device_t device;
    unsigned char addr;
    unsigned char off;
} hermes_device_i2c_slave_t;

typedef hermes_handle_t hermes_device_i2c_slave_handle_t;
HERMES_DEVICE_KIND(i2c_slave);

/* Mandatory abort: it stops answering (the TWI off). */
static inline void device_i2c_slave_abort(hermes_device_i2c_slave_handle_t h_s) {
    hermes_device_i2c_slave_t *s = HERMES_DEVICE_GET(hermes_device_i2c_slave_t, i2c_slave, h_s, "abort");
    hermes_i2c_disable();
    s->off = 1;
}

static inline void device_i2c_slave_describe(hermes_device_i2c_slave_handle_t h_s, hermes_device_desc_t *o) {
    hermes_device_i2c_slave_t *s = HERMES_DEVICE_GET(hermes_device_i2c_slave_t, i2c_slave, h_s, "describe");
    o->pins[0] = HERMES_BOARD_I2C_SDA_PIN;
    o->pins[1] = HERMES_BOARD_I2C_SCL_PIN;
    o->bytes = sizeof(hermes_device_i2c_slave_t);
    HERMES_DEVICE_VALUE(o, "addr", s->addr);
    (void)s;
}

HERMES_DEVICE_CLASS(i2c_slave, hermes_device_i2c_slave_handle_t, 1);

static inline hermes_device_i2c_slave_handle_t i2c_slave(long addr) {
    if (addr < 0x08 || addr > 0x77) {
        HERMES_DIAG("hermes_i2c: i2c_slave(%ld): a slave address is 0x08-0x77 (8-119): the rest are "
                    "reserved\n", addr);
        hermes_fatal();
    }
    hermes_lock();
    hermes_device_pin_claim(HERMES_BOARD_I2C_SDA_PIN, HERMES_PSTR("i2c_slave"), addr);
    hermes_device_pin_claim(HERMES_BOARD_I2C_SCL_PIN, HERMES_PSTR("i2c_slave"), addr);
    hermes_device_i2c_slave_t *s = hermes_runtime_alloc(sizeof(hermes_device_i2c_slave_t));
    if (!s) {
        hermes_unlock();
        HERMES_DIAG("hermes_i2c: i2c_slave(%ld): out of memory (%u bytes)\n", addr,
                    (unsigned)sizeof(hermes_device_i2c_slave_t));
        hermes_fatal();
    }
    s->addr = (unsigned char)addr;
    s->off = 0;
    hermes_device_register_or_abort(&s->device, &hermes_device_class_i2c_slave);
    hermes_unlock();
    hermes_i2c_slave_init((uint8_t)addr);
    return hermes_device_handle(&s->device);
}

static inline void hermes_i2c_slave_release(hermes_device_t *d) {
    (void)d;
    hermes_i2c_disable();
}

static inline void device_i2c_slave_destroy(hermes_device_i2c_slave_handle_t h_s) {
    hermes_device_i2c_slave_t *s = HERMES_DEVICE_GET(hermes_device_i2c_slave_t, i2c_slave, h_s, "destroy");
    hermes_device_destroy(&s->device, hermes_i2c_slave_release);
}

static inline long device_i2c_slave_address(hermes_device_i2c_slave_handle_t h_s) {
    hermes_device_i2c_slave_t *s = HERMES_DEVICE_GET(hermes_device_i2c_slave_t, i2c_slave, h_s, "address");
    return s->addr;
}

static inline long device_i2c_slave_available(hermes_device_i2c_slave_handle_t h_s) {
    (void)HERMES_DEVICE_GET(hermes_device_i2c_slave_t, i2c_slave, h_s, "available");
    return hermes_i2c_slave_available();
}

static inline long device_i2c_slave_read(hermes_device_i2c_slave_handle_t h_s) {
    (void)HERMES_DEVICE_GET(hermes_device_i2c_slave_t, i2c_slave, h_s, "read");
    return hermes_i2c_slave_read();
}

static inline bool device_i2c_slave_reply(hermes_device_i2c_slave_handle_t h_s, long b) {
    hermes_device_i2c_slave_t *s = HERMES_DEVICE_GET(hermes_device_i2c_slave_t, i2c_slave, h_s, "reply");
    if (s->off) return false;
    return hermes_i2c_slave_reply((uint8_t)b) != 0;
}

static inline long device_i2c_slave_replies(hermes_device_i2c_slave_handle_t h_s) {
    (void)HERMES_DEVICE_GET(hermes_device_i2c_slave_t, i2c_slave, h_s, "replies");
    return hermes_i2c_slave_replies();
}

#endif
