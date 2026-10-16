#include "hermes_device.h"
#include "hermes_sched_internal.h" /* hermes_lock: the lists are shared by every core */
#include <stdlib.h>
#if !HERMES_BARE_METAL
#include <sched.h>
#include <stdio.h>
#endif

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on bare metal */

/* Chain of every kind that has had a device registered, in order of first
 * registration. Zero-initialized (.bss), so registering from an
 * __attribute__((constructor)) -- as hermes_thread.c does for the main
 * thread -- is safe. Devices can be created and destroyed from any core, so
 * every access goes under the framework lock (hermes_lock). */
static hermes_device_class_t *g_classes_head = NULL;
static hermes_device_class_t *g_classes_tail = NULL;

/* Device ticks: some linked kind has one; one just linked is due right
 * away (`fresh`); otherwise the earliest tick_next of all of them. */
volatile unsigned char hermes_device_ticks_on = 0;
static volatile unsigned char g_tick_any = 0, g_tick_fresh = 0;
static volatile unsigned long g_tick_next = 0;
/* hermes_device_tick_run is walking the lists (with the lock let go around
 * each tick): hermes_device_destroy waits for it to end. */
static volatile unsigned char g_ticking = 0;

/* --- handles (see "handles" in hermes_device.h) ----------------------------
 * The next serial: only grows (lock held), skipping 0 when it wraps.
 * Given whether the mechanism is on or not (it's cheap, and keeps one
 * layout of hermes_device_t). */
static hermes_serial_t g_next_serial = 1;

hermes_device_t *hermes_device_main;

const char *hermes_device_name(const hermes_device_t *d) {
#if !HERMES_BARE_METAL
    if (!d->cls) return "core"; /* a core's base context: only on the host */
#endif
    return d == hermes_device_main ? "main" : d->cls->name;
}

/* "gpio.write()" for a kind's method; "device_name()" for a generic one */
static void handle_where(const hermes_device_class_t *cls, const char *kind_P, const char *method_P) {
    if (cls) HERMES_DIAG("%" HERMES_PS ".", kind_P);
    HERMES_DIAG("%" HERMES_PS "(): ", method_P);
    (void)cls; (void)kind_P; (void)method_P;
}

void hermes_handle_gone(const hermes_device_class_t *cls, const char *kind_P, const char *method_P) {
    handle_where(cls, kind_P, method_P);
    HERMES_DIAG("no %" HERMES_PS " here: it was destroyed (the variable is left empty) or never created\n",
                kind_P);
    hermes_fatal();
}

#ifdef HERMES_HANDLES
/* Lock held: the device with serial h, in kind cls's list (NULL: any kind). */
static hermes_device_t *find_serial(hermes_handle_t h, const hermes_device_class_t *cls) {
    if (h == HERMES_HANDLE_NONE) return NULL;
    for (const hermes_device_class_t *c = cls ? cls : g_classes_head; c; c = cls ? NULL : c->next_class)
        for (hermes_device_t *it = c->head; it; it = it->next)
            if (it->serial == h) return it;
    return NULL;
}

hermes_device_t *hermes_handle_peek(hermes_handle_t h) {
    hermes_lock();
    hermes_device_t *d = find_serial(h, NULL);
    hermes_unlock();
    return d;
}

hermes_device_t *hermes_handle_get(hermes_handle_t h, const hermes_device_class_t *cls,
                                   const char *kind_P, const char *method_P) {
    hermes_lock();
    hermes_device_t *d = find_serial(h, cls);
    hermes_device_t *other = d ? NULL : find_serial(h, NULL); /* of another kind? */
    hermes_unlock();
    if (d) return d;
    if (h == HERMES_HANDLE_NONE) hermes_handle_gone(cls, kind_P, method_P);
    handle_where(cls, kind_P, method_P);
    if (other) {
        HERMES_DIAG("that is a %s device, not a %" HERMES_PS "\n", other->cls->name, kind_P);
    } else {
        HERMES_DIAG("this %" HERMES_PS " was destroyed: a copy of its handle?\n", kind_P);
    }
    hermes_fatal();
}
#endif

hermes_device_status_t hermes_device_register(hermes_device_t *d,
                                              hermes_device_class_t *cls) {
    if (!cls->abort) {
        return HERMES_DEVICE_ERR_NO_ABORT;
    }
    hermes_lock();
    if (cls->max_instances != 0 && cls->count >= cls->max_instances) {
        hermes_unlock();
        return HERMES_DEVICE_ERR_CLASS_LIMIT;
    }
    for (hermes_device_t *it = cls->head; it; it = it->next) {
        if (it == d) { hermes_unlock(); return HERMES_DEVICE_ERR_DUPLICATE; }
    }

    if (!cls->linked) {
        cls->linked = 1;
        cls->next_class = NULL;
        if (g_classes_tail) g_classes_tail->next_class = cls;
        else g_classes_head = cls;
        g_classes_tail = cls;
        if (cls->tick) {
            cls->tick_next = 0;
            g_tick_any = 1;
            g_tick_fresh = 1;
        }
    }

    d->cls = cls;
    d->serial = g_next_serial;
    if (++g_next_serial == 0) g_next_serial = 1;
    d->next = NULL;
    if (cls->tail) cls->tail->next = d;
    else cls->head = d;
    cls->tail = d;
    cls->count++;
    hermes_unlock();
    return HERMES_DEVICE_OK;
}

void hermes_device_register_or_abort(hermes_device_t *d,
                                     hermes_device_class_t *cls) {
    hermes_device_status_t st = hermes_device_register(d, cls);
    if (st == HERMES_DEVICE_OK) return;
    if (st == HERMES_DEVICE_ERR_NO_ABORT) {
        HERMES_DIAG("hermes_device: kind '%s' doesn't define 'abort' (mandatory: every device "
                    "must be able to be deactivated; see HERMES_DEVICE_CLASS)\n", cls->name);
    } else if (st == HERMES_DEVICE_ERR_CLASS_LIMIT) {
        HERMES_DIAG("hermes_device: limit of kind '%s' reached (at most %d)\n",
                    cls->name, (int)cls->max_instances);
    } else {
        HERMES_DIAG("hermes_device: a %s device was registered twice\n", cls->name);
    }
    hermes_fatal();
}

void hermes_device_abort_all(void) {
    static unsigned char running = 0;
    hermes_lock();
    if (running) { hermes_unlock(); return; }
    running = 1;
    for (hermes_device_class_t *c = g_classes_head; c; c = c->next_class) {
        /* read `next` first: an abort must not depend on the device staying
         * linked (a future abort may also unregister it) */
        hermes_device_t *it = c->head;
        while (it) {
            hermes_device_t *next = it->next;
            c->abort(it);
            it = next;
        }
    }
    running = 0;
    hermes_unlock();
}

volatile unsigned char hermes_fatal_started = 0;

void hermes_fatal(void) {
    hermes_fatal_started = 1; /* see hermes_sched_internal.h */
    hermes_device_abort_all();
#if !HERMES_BARE_METAL
    fflush(NULL); /* abort() doesn't flush stdio: keep the trace of what the aborts did */
#endif
    abort();
}

int hermes_device_unregister(hermes_device_t *d) {
    hermes_device_class_t *cls = d->cls;
    if (!cls) return 0;
    hermes_lock();
    hermes_device_t *prev = NULL;
    for (hermes_device_t *it = cls->head; it; prev = it, it = it->next) {
        if (it != d) continue;
        if (prev) prev->next = it->next;
        else cls->head = it->next;
        if (cls->tail == it) cls->tail = prev;
        cls->count--;
        d->next = NULL;
        hermes_unlock();
        return 1;
    }
    hermes_unlock();
    return 0;
}

int hermes_device_count(void) {
    int n = 0;
    hermes_lock();
    for (hermes_device_class_t *c = g_classes_head; c; c = c->next_class) n += c->count;
    hermes_unlock();
    return n;
}

hermes_device_t *hermes_device_at(int i) {
    if (i < 0) return NULL;
    hermes_device_t *found = NULL;
    hermes_lock();
    for (hermes_device_class_t *c = g_classes_head; c; c = c->next_class) {
        if (i < (int)c->count) { found = hermes_device_class_at(c, i); break; }
        i -= (int)c->count;
    }
    hermes_unlock();
    return found;
}

int hermes_device_count_class(const hermes_device_class_t *cls) {
    return cls->count;
}

hermes_device_t *hermes_device_class_at(const hermes_device_class_t *cls, int i) {
    if (i < 0) return NULL;
    hermes_lock();
    hermes_device_t *it = cls->head;
    while (it && i-- > 0) it = it->next;
    hermes_unlock();
    return it;
}

/* --- device ticks (see hermes_device.h) ------------------------------------ */

static int reached(unsigned long now, unsigned long at) { return (long)(now - at) >= 0; }

int hermes_device_tick_due(unsigned long now) {
    return g_tick_any && (g_tick_fresh || reached(now, g_tick_next));
}

/* The lock is taken to walk the lists and let go around every call (a
 * tick may take it; on bare metal holding it would mask interrupts). The
 * devices stay meanwhile: hermes_device_destroy waits for g_ticking. */
void hermes_device_tick_run(unsigned long now) {
    hermes_lock();
    g_ticking = 1;
    g_tick_fresh = 0;
    int any = 0;
    unsigned long next_all = 0;
    for (hermes_device_class_t *c = g_classes_head; c; c = c->next_class) {
        if (!c->tick) continue;
        if (c->tick_next == 0 || reached(now, c->tick_next)) {
            c->tick_next = now + (c->tick_ms ? c->tick_ms : 1);
            if (c->tick_next == 0) c->tick_next = 1; /* 0 = "never ticked" */
            for (hermes_device_t *d = c->head; d; d = d->next) {
                hermes_unlock();
                c->tick(d);
                hermes_lock();
            }
        }
        if (!any || (long)(c->tick_next - next_all) < 0) next_all = c->tick_next;
        any = 1;
    }
    g_tick_next = next_all;
    g_ticking = 0;
    hermes_unlock();
}

/* --- describe, destroy, conflicts (see hermes_device.h) ----------------------- */

#ifdef HERMES_DEVICES
void hermes_device_desc_value(hermes_device_desc_t *o, const char *key, long value) {
    if (o->nfields >= HERMES_DEVICE_FIELDS) return;
    hermes_device_field_t *f = &o->field[o->nfields++];
    f->key = key; f->text = 0; f->value = value;
}

void hermes_device_desc_text(hermes_device_desc_t *o, const char *key, const char *text) {
    if (o->nfields >= HERMES_DEVICE_FIELDS) return;
    hermes_device_field_t *f = &o->field[o->nfields++];
    f->key = key; f->text = text; f->value = 0;
}
#endif

void hermes_device_describe(hermes_device_t *d, hermes_device_desc_t *out) {
    out->pins[0] = out->pins[1] = out->pins[2] = -1;
    out->nfields = 0;
    out->bytes = 0;
    if (d->cls && d->cls->describe) d->cls->describe(d, out);
}

void hermes_device_destroy(hermes_device_t *d, void (*release)(hermes_device_t *d)) {
    hermes_lock();
    /* The OS's device ticks let go of the lock around every call and then
     * follow the device's link: it must not go away meanwhile. Only on the
     * host can this wait (another core's OS); on bare metal a thread never
     * runs while its core's OS works. */
    while (g_ticking) {
        hermes_unlock();
#if !HERMES_BARE_METAL
        sched_yield();
#endif
        hermes_lock();
    }
    if (release) release(d);
    hermes_device_unregister(d);
    hermes_unlock();
    hermes_runtime_free(d);
}


hermes_device_t *hermes_device_pin_user(int pin) {
    if (pin < 0) return NULL;
    for (hermes_device_class_t *c = g_classes_head; c; c = c->next_class) {
        for (hermes_device_t *d = c->head; d; d = d->next) {
            hermes_device_desc_t desc;
            hermes_device_describe(d, &desc);
            if (desc.pins[0] == pin || desc.pins[1] == pin || desc.pins[2] == pin) return d;
        }
    }
    return NULL;
}

void hermes_device_pin_claim(int pin, const char *ctor_P, long arg) {
    hermes_device_t *user = hermes_device_pin_user(pin);
    if (!user) return;
    hermes_unlock();
    HERMES_DIAG("%" HERMES_PS "(%ld): pin %d is already used by a %s device\n",
                ctor_P, arg, pin, hermes_device_class_name(user));
    (void)ctor_P; (void)arg;
    hermes_fatal();
}

/* --- dump and memory ------------------------------------------------------------
 * Out like print(): stdout on the host; hermes_putchar on bare metal (the
 * UART driver's, weak: nothing without it). Constant texts from flash on
 * AVR. Each line between hermes_preempt_off/on (weak: no runtime, no
 * guard), as print() does; the lock is never held while writing (on bare
 * metal it masks interrupts: millis would lose ticks). */
#if HERMES_BARE_METAL
extern void hermes_putchar(char c) __attribute__((weak));
static void out_c(char c) { if (hermes_putchar) hermes_putchar(c); }
#else
static void out_c(char c) { putchar(c); }
#endif
extern void hermes_preempt_off(void) __attribute__((weak));
extern void hermes_preempt_on(void) __attribute__((weak));

static void out_ram(const char *s) { while (*s) out_c(*s++); }

static void out_flash(const char *s) {
#if defined(__AVR__)
    char c;
    while ((c = (char)pgm_read_byte(s++))) out_c(c);
#else
    out_ram(s);
#endif
}

static void out_long(long v) {
    char buf[12];
    int i = 0;
    unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    do { buf[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) out_c('-');
    while (i > 0) out_c(buf[--i]);
}

static void line_begin(void) { if (hermes_preempt_off) hermes_preempt_off(); }
static void line_end(void) {
    out_c('\n');
    if (hermes_preempt_on) hermes_preempt_on();
}

/* "N device(s), B B" */
static void out_count(long n, unsigned long bytes) {
    out_long(n);
    out_flash(n == 1 ? HERMES_DEVICE_STR(" device, ") : HERMES_DEVICE_STR(" devices, "));
    out_long((long)bytes);
    out_flash(HERMES_DEVICE_STR(" B"));
}

/* The i-th device of `c`, described (lock taken and let go here). */
static int nth_desc(hermes_device_class_t *c, int i, hermes_device_desc_t *desc, const char **name) {
    hermes_lock();
    hermes_device_t *d = c->head;
    while (d && i-- > 0) d = d->next;
    if (d) { hermes_device_describe(d, desc); *name = hermes_device_name(d); }
    hermes_unlock();
    return d != NULL;
}

void hermes_device_dump(void) {
    long total_n = 0;
    unsigned long total_b = 0;
    /* kinds are never unlinked: walking their chain needs no lock */
    for (hermes_device_class_t *c = g_classes_head; c; c = c->next_class) {
        hermes_device_desc_t desc;
        const char *name;
        long n = 0;
        unsigned long bytes = 0;
        while (nth_desc(c, (int)n, &desc, &name)) { n++; bytes += desc.bytes; }
        if (n == 0) continue; /* every one destroyed */
        total_n += n;
        total_b += bytes;
        line_begin();
        out_ram(c->name);
        out_flash(HERMES_DEVICE_STR(": "));
        out_count(n, bytes);
        line_end();
        for (int i = 0; nth_desc(c, i, &desc, &name); i++) {
            line_begin();
            out_flash(HERMES_DEVICE_STR("  #"));
            out_long(i);
            if (name && c->name && name != c->name) {
                const char *a = name, *b = c->name; /* instance name, if not the kind's */
                while (*a && *a == *b) { a++; b++; }
                if (*a != *b) { out_c(' '); out_ram(name); }
            }
            if (desc.pins[0] >= 0) {
                out_flash(desc.pins[1] >= 0 ? HERMES_DEVICE_STR(" pins=") : HERMES_DEVICE_STR(" pin="));
                out_long(desc.pins[0]);
                if (desc.pins[1] >= 0) { out_c(','); out_long(desc.pins[1]); }
                if (desc.pins[2] >= 0) { out_c(','); out_long(desc.pins[2]); }
            }
            for (int f = 0; f < desc.nfields; f++) {
                out_c(' ');
                out_flash(desc.field[f].key);
                out_c('=');
                if (desc.field[f].text) out_flash(desc.field[f].text);
                else out_long(desc.field[f].value);
            }
            line_end();
        }
    }
    line_begin();
    out_flash(HERMES_DEVICE_STR("total: "));
    out_count(total_n, total_b);
    line_end();
}

unsigned long hermes_device_memory(void) {
    unsigned long bytes = 0;
    hermes_lock();
    for (hermes_device_class_t *c = g_classes_head; c; c = c->next_class) {
        for (hermes_device_t *d = c->head; d; d = d->next) {
            hermes_device_desc_t desc;
            hermes_device_describe(d, &desc);
            bytes += desc.bytes;
        }
    }
    hermes_unlock();
    return bytes;
}
