#ifndef HERMES_SPI_H
#define HERMES_SPI_H

/* `import hermes_spi`: the SPI bus (master) as built-in (core) devices,
 * like hermes_i2c (asked by the user, 2026-10-16). hepyc links the SPI
 * driver (src/framework/drivers/hermes_drv_spi.h) and the runtime.
 *
 *   device bus = spi(8000000, 0)  the bus: clock (the fastest the chip
 *                                 has, not above it) and mode 0-3 (CPOL,
 *                                 CPHA); holds SCK, MOSI and MISO
 *   bus.frequency()               the clock actually used
 *   device chip = bus.at(10)      a chip on the bus, by its chip-select pin
 *                                 (any digital pin; held by the device)
 *   chip.begin()                  select it (CS low) for a transaction
 *   long r = chip.transfer(b)     one byte out, one in (outside begin/end:
 *                                 a transaction of its own)
 *   chip.end()                    release it (CS high)
 *   chip.abort() / bus.abort()    release / bus off
 *   chip.destroy() / bus.destroy() (the bus: a fatal error while it has
 *                                 chips)
 *
 * The bus's own SS pin (Uno D10, Mega D53) is driven as an output by the
 * driver but not held: it's often a chip's select (the Arduino Ethernet
 * shield's W5500 is on D10). A transaction (begin .. end) isn't split by
 * the OS's preemption on bare metal (another thread could select another
 * chip in the middle) -- keep it short. AVR + the host's mock (virtual
 * chips); not yet on the ESP32-C3 (hepyc reports it). */

#include "hermes_device.h"
#include "hermes_drv_spi.h"
#include "hermes_drv_gpio.h"
#include "hermes_sched_internal.h" /* hermes_runtime_alloc, hermes_lock */
#include "hermes_board.h"
#include "hermes_diag.h"
#include <stdbool.h>

#ifndef HERMES_BOARD_SPI_SCK_PIN
#define HERMES_BOARD_SPI_SCK_PIN -1
#define HERMES_BOARD_SPI_MOSI_PIN -1
#define HERMES_BOARD_SPI_MISO_PIN -1
#endif

typedef struct {
    hermes_device_t device;
    long hz;
    unsigned char mode;
} hermes_device_spi_t;

typedef struct {
    hermes_device_t device;
    hermes_device_spi_t *bus;
    signed char cs;
    unsigned char selected;
} hermes_device_spi_dev_t;

typedef hermes_handle_t hermes_device_spi_handle_t;
typedef hermes_handle_t hermes_device_spi_dev_handle_t;
HERMES_DEVICE_KIND(spi);
HERMES_DEVICE_KIND(spi_dev);

/* --- the bus ---------------------------------------------------------------- */

static inline void device_spi_abort(hermes_device_spi_handle_t h) {
    (void)HERMES_DEVICE_GET(hermes_device_spi_t, spi, h, "abort");
    hermes_spi_disable();
}

static inline void device_spi_describe(hermes_device_spi_handle_t h, hermes_device_desc_t *o) {
    hermes_device_spi_t *b = HERMES_DEVICE_GET(hermes_device_spi_t, spi, h, "describe");
    o->pins[0] = HERMES_BOARD_SPI_SCK_PIN;
    o->pins[1] = HERMES_BOARD_SPI_MOSI_PIN;
    o->pins[2] = HERMES_BOARD_SPI_MISO_PIN;
    o->bytes = sizeof(hermes_device_spi_t);
    HERMES_DEVICE_VALUE(o, "hz", b->hz);
    HERMES_DEVICE_VALUE(o, "mode", b->mode); (void)b;
}

HERMES_DEVICE_CLASS(spi, hermes_device_spi_handle_t, 1);

static inline hermes_device_spi_handle_t spi(long hz, long mode) {
    if (hz < 1000 || mode < 0 || mode > 3) {
        HERMES_DIAG("hermes_spi: spi(%ld, %ld): the clock must be at least 1000 Hz and the mode 0-3\n", hz, mode);
        hermes_fatal();
    }
    hermes_lock();
    hermes_device_pin_claim(HERMES_BOARD_SPI_SCK_PIN, HERMES_PSTR("spi"), hz);
    hermes_device_pin_claim(HERMES_BOARD_SPI_MOSI_PIN, HERMES_PSTR("spi"), hz);
    hermes_device_pin_claim(HERMES_BOARD_SPI_MISO_PIN, HERMES_PSTR("spi"), hz);
    hermes_device_spi_t *b = hermes_runtime_alloc(sizeof(hermes_device_spi_t));
    if (!b) {
        hermes_unlock();
        HERMES_DIAG("hermes_spi: spi(%ld): out of memory (%u bytes)\n", hz, (unsigned)sizeof(hermes_device_spi_t));
        hermes_fatal();
    }
    b->mode = (unsigned char)mode;
    b->hz = (long)hermes_spi_init((uint32_t)hz, (uint8_t)mode);
    hermes_device_register_or_abort(&b->device, &hermes_device_class_spi);
    hermes_unlock();
    return hermes_device_handle(&b->device);
}

static inline long device_spi_frequency(hermes_device_spi_handle_t h) {
    return HERMES_DEVICE_GET(hermes_device_spi_t, spi, h, "frequency")->hz;
}

/* --- a chip ------------------------------------------------------------------ */

static inline void device_spi_dev_abort(hermes_device_spi_dev_handle_t h) {
    hermes_device_spi_dev_t *c = HERMES_DEVICE_GET(hermes_device_spi_dev_t, spi_dev, h, "abort");
    if (c->selected) { hermes_spi_select(c->cs, 0); hermes_spi_end(); c->selected = 0; }
}

static inline void device_spi_dev_describe(hermes_device_spi_dev_handle_t h, hermes_device_desc_t *o) {
    hermes_device_spi_dev_t *c = HERMES_DEVICE_GET(hermes_device_spi_dev_t, spi_dev, h, "describe");
    o->pins[0] = c->cs;
    o->bytes = sizeof(hermes_device_spi_dev_t);
}

HERMES_DEVICE_CLASS(spi_dev, hermes_device_spi_dev_handle_t, 0);

/* Chips of bus b (lock held). */
static inline int hermes_spi_chips_of(hermes_device_spi_t *b) {
    int n = 0;
    for (hermes_device_t *it = hermes_device_class_spi_dev.head; it; it = it->next)
        if (((hermes_device_spi_dev_t *)it)->bus == b) n++;
    return n;
}

static inline hermes_device_spi_dev_handle_t device_spi_at(hermes_device_spi_handle_t h, long cs) {
    hermes_device_spi_t *b = HERMES_DEVICE_GET(hermes_device_spi_t, spi, h, "at");
    hermes_lock();
    hermes_device_pin_claim((int)cs, HERMES_PSTR("at"), cs);
    hermes_device_spi_dev_t *c = hermes_runtime_alloc(sizeof(hermes_device_spi_dev_t));
    if (!c) {
        hermes_unlock();
        HERMES_DIAG("hermes_spi: at(%ld): out of memory (%u bytes)\n", cs, (unsigned)sizeof(hermes_device_spi_dev_t));
        hermes_fatal();
    }
    c->bus = b;
    c->cs = (signed char)cs;
    c->selected = 0;
    hermes_spi_cs_setup((int)cs);
    hermes_device_register_or_abort(&c->device, &hermes_device_class_spi_dev);
    hermes_unlock();
    return hermes_device_handle(&c->device);
}

static inline void device_spi_dev_begin(hermes_device_spi_dev_handle_t h) {
    hermes_device_spi_dev_t *c = HERMES_DEVICE_GET(hermes_device_spi_dev_t, spi_dev, h, "begin");
    if (c->selected) return;
    hermes_spi_begin();
    hermes_spi_select(c->cs, 1);
    c->selected = 1;
}

static inline void device_spi_dev_end(hermes_device_spi_dev_handle_t h) {
    hermes_device_spi_dev_t *c = HERMES_DEVICE_GET(hermes_device_spi_dev_t, spi_dev, h, "end");
    if (!c->selected) return;
    hermes_spi_select(c->cs, 0);
    c->selected = 0;
    hermes_spi_end();
}

static inline long device_spi_dev_transfer(hermes_device_spi_dev_handle_t h, long b) {
    hermes_device_spi_dev_t *c = HERMES_DEVICE_GET(hermes_device_spi_dev_t, spi_dev, h, "transfer");
    if (c->selected) return hermes_spi_transfer((uint8_t)b);
    hermes_spi_begin();
    hermes_spi_select(c->cs, 1);
    long r = hermes_spi_transfer((uint8_t)b);
    hermes_spi_select(c->cs, 0);
    hermes_spi_end();
    return r;
}

/* The chip-select pin of a chip (for modules built on top: w5500...). */
static inline int hermes_spi_dev_cs(hermes_device_spi_dev_handle_t h, const char *method_P) {
    return ((hermes_device_spi_dev_t *)hermes_handle_get(h, &hermes_device_class_spi_dev,
                                                         HERMES_PSTR("spi_dev"), method_P))->cs;
}

/* destroy */
static inline void hermes_spi_dev_release(hermes_device_t *d) {
    device_spi_dev_abort(hermes_device_handle(d));
    hermes_gpio_mode(((hermes_device_spi_dev_t *)d)->cs, HERMES_GPIO_INPUT);
}

static inline void device_spi_dev_destroy(hermes_device_spi_dev_handle_t h) {
    hermes_device_spi_dev_t *c = HERMES_DEVICE_GET(hermes_device_spi_dev_t, spi_dev, h, "destroy");
    hermes_device_destroy(&c->device, hermes_spi_dev_release);
}

static inline void hermes_spi_release(hermes_device_t *d) {
    device_spi_abort(hermes_device_handle(d));
}

static inline void device_spi_destroy(hermes_device_spi_handle_t h) {
    hermes_device_spi_t *b = HERMES_DEVICE_GET(hermes_device_spi_t, spi, h, "destroy");
    hermes_lock();
    int n = hermes_spi_chips_of(b);
    hermes_unlock();
    if (n) {
        HERMES_DIAG("hermes_spi: destroy() while %d chips of the bus (bus.at) exist: destroy them first\n", n);
        hermes_fatal();
    }
    hermes_device_destroy(&b->device, hermes_spi_release);
}

#endif
