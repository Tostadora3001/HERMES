/* I2C driver, common part: see hermes_drv_i2c.h. */
#include "hermes_drv_i2c.h"

#ifndef F_CPU
#define F_CPU 16000000UL /* host: the mock does the Uno's arithmetic */
#endif

uint32_t hermes_i2c_divider(uint32_t hz, uint8_t *twbr, uint8_t *twps) {
    /* the smallest prescaler whose TWBR fits; TWBR rounded up so the
     * frequency never goes above what was asked */
    for (uint8_t ps = 0; ps < 4; ps++) {
        uint32_t mult = 2UL << (2 * ps); /* 2 * 4^ps */
        uint32_t need = F_CPU / hz;
        uint32_t br = need > 16 ? (need - 16 + mult - 1) / mult : 0;
        if (br > 255) continue;
        *twbr = (uint8_t)br;
        *twps = ps;
        return F_CPU / (16 + mult * br);
    }
    *twbr = 255;
    *twps = 3;
    return F_CPU / (16 + 128UL * 255);
}

const char *hermes_i2c_error_name(int err) {
    switch (err) {
        case HERMES_I2C_OK: return "ok";
        case HERMES_I2C_NACK_ADDR: return "nack_addr";
        case HERMES_I2C_NACK_DATA: return "nack_data";
        case HERMES_I2C_TIMEOUT: return "timeout";
        case HERMES_I2C_BUS_ERROR: return "bus_error";
        case HERMES_I2C_OFF: return "off";
        default: return "?";
    }
}

/* Every transaction ends with a STOP, also after an error (the bus is
 * left free), except a timeout: the platform already reset the TWI. */
static int finish(int st) {
    if (st != HERMES_I2C_TIMEOUT && st != HERMES_I2C_OFF) hermes_i2c_stop();
    hermes_i2c_unlock();
    return st;
}

int hermes_i2c_probe(uint8_t addr) {
    hermes_i2c_lock();
    return finish(hermes_i2c_start(addr, 0));
}

int hermes_i2c_write_byte(uint8_t addr, uint8_t b) {
    hermes_i2c_lock();
    int st = hermes_i2c_start(addr, 0);
    if (st == HERMES_I2C_OK) st = hermes_i2c_send(b);
    return finish(st);
}

int hermes_i2c_read_byte(uint8_t addr, uint8_t *out) {
    hermes_i2c_lock();
    int st = hermes_i2c_start(addr, 1);
    if (st == HERMES_I2C_OK) st = hermes_i2c_recv(0, out);
    return finish(st);
}

int hermes_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t v) {
    hermes_i2c_lock();
    int st = hermes_i2c_start(addr, 0);
    if (st == HERMES_I2C_OK) st = hermes_i2c_send(reg);
    if (st == HERMES_I2C_OK) st = hermes_i2c_send(v);
    return finish(st);
}

int hermes_i2c_write_reg16(uint8_t addr, uint8_t reg, uint16_t v) {
    hermes_i2c_lock();
    int st = hermes_i2c_start(addr, 0);
    if (st == HERMES_I2C_OK) st = hermes_i2c_send(reg);
    if (st == HERMES_I2C_OK) st = hermes_i2c_send((uint8_t)(v >> 8));
    if (st == HERMES_I2C_OK) st = hermes_i2c_send((uint8_t)v);
    return finish(st);
}

/* write the register number, repeated START, read n bytes (n = 1 or 2) */
static int read_regs(uint8_t addr, uint8_t reg, uint8_t *b, int n) {
    hermes_i2c_lock();
    int st = hermes_i2c_start(addr, 0);
    if (st == HERMES_I2C_OK) st = hermes_i2c_send(reg);
    if (st == HERMES_I2C_OK) st = hermes_i2c_start(addr, 1);
    for (int i = 0; i < n && st == HERMES_I2C_OK; i++) st = hermes_i2c_recv(i < n - 1, &b[i]);
    return finish(st);
}

int hermes_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *out) {
    return read_regs(addr, reg, out, 1);
}

int hermes_i2c_read_reg16(uint8_t addr, uint8_t reg, uint16_t *out) {
    uint8_t b[2];
    int st = read_regs(addr, reg, b, 2);
    if (st == HERMES_I2C_OK) *out = (uint16_t)((b[0] << 8) | b[1]);
    return st;
}
