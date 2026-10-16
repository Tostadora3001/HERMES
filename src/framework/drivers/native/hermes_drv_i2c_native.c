/* I2C driver, host (native) implementation: a mock. See hermes_drv_i2c.h.
 * Master: virtual chips with an auto-incrementing register pointer (an
 * EEPROM of 256 bytes at 0x50 from the start). Slave: rings like the
 * AVR's, fed by a virtual master (hermes_i2c_mock_master_*). */
#include "hermes_drv_i2c.h"
#include <pthread.h>
#include <string.h>

#define MOCK_CHIPS 8

typedef struct {
    int addr;      /* -1 = free slot */
    int size;
    int ptr;
    uint8_t mem[256];
} chip_t;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static chip_t g_chips[MOCK_CHIPS];
static int g_ready;          /* slots initialized, default EEPROM added */
static int g_mode;           /* 0 off, 1 master, 2 slave */
static chip_t *g_cur;        /* chip of the transaction in progress */
static int g_cur_read, g_cur_first;
static int g_slave_addr = -1;

static uint8_t g_rx[HERMES_I2C_SLAVE_RX], g_tx[HERMES_I2C_SLAVE_TX];
static unsigned g_rx_head, g_rx_tail, g_tx_head, g_tx_tail;

static int add_locked(int addr, int size);

static void setup(void) {
    if (g_ready) return;
    g_ready = 1;
    for (int i = 0; i < MOCK_CHIPS; i++) g_chips[i].addr = -1;
    add_locked(0x50, 256);
}

static chip_t *find(int addr) {
    for (int i = 0; i < MOCK_CHIPS; i++) if (g_chips[i].addr == addr) return &g_chips[i];
    return NULL;
}

static int add_locked(int addr, int size) {
    if (addr < 0 || addr > 127 || size < 1 || size > 256) return 0;
    chip_t *c = find(addr);
    for (int i = 0; !c && i < MOCK_CHIPS; i++) if (g_chips[i].addr < 0) c = &g_chips[i];
    if (!c) return 0;
    c->addr = addr;
    c->size = size;
    c->ptr = 0;
    memset(c->mem, 0, sizeof(c->mem));
    return 1;
}

int hermes_i2c_mock_add(int addr, int size) {
    pthread_mutex_lock(&g_mutex);
    setup();
    int ok = add_locked(addr, size);
    pthread_mutex_unlock(&g_mutex);
    return ok;
}

void hermes_i2c_mock_remove(int addr) {
    pthread_mutex_lock(&g_mutex);
    setup();
    chip_t *c = find(addr);
    if (c) c->addr = -1;
    pthread_mutex_unlock(&g_mutex);
}

void hermes_i2c_lock(void) { pthread_mutex_lock(&g_mutex); }
void hermes_i2c_unlock(void) { pthread_mutex_unlock(&g_mutex); }

/* --- master ------------------------------------------------------------------ */

uint32_t hermes_i2c_master_init(uint32_t hz) {
    uint8_t br, ps;
    pthread_mutex_lock(&g_mutex);
    setup();
    g_mode = 1;
    g_cur = NULL;
    pthread_mutex_unlock(&g_mutex);
    return hermes_i2c_divider(hz, &br, &ps);
}

void hermes_i2c_disable(void) {
    g_mode = 0;
    g_cur = NULL;
    g_slave_addr = -1;
}

int hermes_i2c_start(uint8_t addr, int read) {
    if (g_mode != 1) return HERMES_I2C_BUS_ERROR;
    g_cur = find(addr);
    if (!g_cur) return HERMES_I2C_NACK_ADDR;
    g_cur_read = read;
    g_cur_first = 1;
    return HERMES_I2C_OK;
}

int hermes_i2c_send(uint8_t b) {
    if (!g_cur || g_cur_read) return HERMES_I2C_BUS_ERROR;
    if (g_cur_first) {
        g_cur->ptr = b % g_cur->size; /* the first byte sets the pointer */
        g_cur_first = 0;
    } else {
        g_cur->mem[g_cur->ptr] = b;
        g_cur->ptr = (g_cur->ptr + 1) % g_cur->size;
    }
    return HERMES_I2C_OK;
}

int hermes_i2c_recv(int ack, uint8_t *b) {
    (void)ack;
    if (!g_cur || !g_cur_read) return HERMES_I2C_BUS_ERROR;
    *b = g_cur->mem[g_cur->ptr];
    g_cur->ptr = (g_cur->ptr + 1) % g_cur->size;
    return HERMES_I2C_OK;
}

void hermes_i2c_stop(void) { g_cur = NULL; }

/* --- slave -------------------------------------------------------------------- */

void hermes_i2c_slave_init(uint8_t addr) {
    pthread_mutex_lock(&g_mutex);
    setup();
    g_mode = 2;
    g_slave_addr = addr;
    g_rx_head = g_rx_tail = g_tx_head = g_tx_tail = 0;
    pthread_mutex_unlock(&g_mutex);
}

int hermes_i2c_slave_available(void) {
    pthread_mutex_lock(&g_mutex);
    int n = (int)((g_rx_head - g_rx_tail) & (HERMES_I2C_SLAVE_RX - 1));
    pthread_mutex_unlock(&g_mutex);
    return n;
}

int hermes_i2c_slave_read(void) {
    int b = -1;
    pthread_mutex_lock(&g_mutex);
    if (g_rx_tail != g_rx_head) {
        b = g_rx[g_rx_tail];
        g_rx_tail = (g_rx_tail + 1) & (HERMES_I2C_SLAVE_RX - 1);
    }
    pthread_mutex_unlock(&g_mutex);
    return b;
}

int hermes_i2c_slave_reply(uint8_t b) {
    int ok = 0;
    pthread_mutex_lock(&g_mutex);
    unsigned next = (g_tx_head + 1) & (HERMES_I2C_SLAVE_TX - 1);
    if (next != g_tx_tail) { g_tx[g_tx_head] = b; g_tx_head = next; ok = 1; }
    pthread_mutex_unlock(&g_mutex);
    return ok;
}

int hermes_i2c_slave_replies(void) {
    pthread_mutex_lock(&g_mutex);
    int n = (int)((g_tx_head - g_tx_tail) & (HERMES_I2C_SLAVE_TX - 1));
    pthread_mutex_unlock(&g_mutex);
    return n;
}

int hermes_i2c_mock_master_send(int addr, int b) {
    int r = -1;
    pthread_mutex_lock(&g_mutex);
    if (g_mode == 2 && addr == g_slave_addr) {
        unsigned next = (g_rx_head + 1) & (HERMES_I2C_SLAVE_RX - 1);
        if (next != g_rx_tail) { g_rx[g_rx_head] = (uint8_t)b; g_rx_head = next; } /* full: dropped */
        r = 0;
    }
    pthread_mutex_unlock(&g_mutex);
    return r;
}

int hermes_i2c_mock_master_read(int addr) {
    int r = -1;
    pthread_mutex_lock(&g_mutex);
    if (g_mode == 2 && addr == g_slave_addr) {
        if (g_tx_tail != g_tx_head) {
            r = g_tx[g_tx_tail];
            g_tx_tail = (g_tx_tail + 1) & (HERMES_I2C_SLAVE_TX - 1);
        } else {
            r = 0xFF;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return r;
}
