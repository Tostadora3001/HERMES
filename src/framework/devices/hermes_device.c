#include "hermes_device.h"
#include <stdlib.h>

/* Same policy as hermes_thread.c: no diagnostics on AVR (no output stream
 * wired to stderr, and fprintf's formatting engine costs ~1.5 KB of flash). */
#if defined(__AVR__)
#define HERMES_DIAG(...) ((void)0)
#else
#include <stdio.h>
#define HERMES_DIAG(...) fprintf(stderr, __VA_ARGS__)
#endif

/* Chain of every kind that has had a device registered, in order of first
 * registration. Zero-initialized (.bss), so registering from an
 * __attribute__((constructor)) -- as hermes_thread.c does for the main
 * thread -- is safe. Only touched from the cooperative scheduler's single
 * logical thread (never from an ISR), so no locking. */
static hermes_device_class_t *g_classes_head = NULL;
static hermes_device_class_t *g_classes_tail = NULL;

hermes_device_status_t hermes_device_register(hermes_device_t *d,
                                              hermes_device_class_t *cls,
                                              const char *name) {
    if (!cls->abort) {
        return HERMES_DEVICE_ERR_NO_ABORT;
    }
    if (cls->max_instances != 0 && cls->count >= cls->max_instances) {
        return HERMES_DEVICE_ERR_CLASS_LIMIT;
    }
    for (hermes_device_t *it = cls->head; it; it = it->next) {
        if (it == d) return HERMES_DEVICE_ERR_DUPLICATE;
    }

    if (!cls->linked) {
        cls->linked = 1;
        cls->next_class = NULL;
        if (g_classes_tail) g_classes_tail->next_class = cls;
        else g_classes_head = cls;
        g_classes_tail = cls;
    }

    d->cls = cls;
    d->name = name;
    d->next = NULL;
    if (cls->tail) cls->tail->next = d;
    else cls->head = d;
    cls->tail = d;
    cls->count++;
    return HERMES_DEVICE_OK;
}

void hermes_device_register_or_abort(hermes_device_t *d,
                                     hermes_device_class_t *cls,
                                     const char *name) {
    hermes_device_status_t st = hermes_device_register(d, cls, name);
    if (st == HERMES_DEVICE_OK) return;
    if (st == HERMES_DEVICE_ERR_NO_ABORT) {
        HERMES_DIAG("hermes_device: el tipo '%s' no define 'abort' (obligatorio: todo device "
                    "debe poder desactivarse; ver HERMES_DEVICE_CLASS)\n", cls->name);
    } else if (st == HERMES_DEVICE_ERR_CLASS_LIMIT) {
        HERMES_DIAG("hermes_device: limite del tipo '%s' alcanzado (%d como maximo)\n",
                    cls->name, (int)cls->max_instances);
    } else {
        HERMES_DIAG("hermes_device: '%s' ya estaba registrado\n", name);
    }
    hermes_fatal();
}

void hermes_device_abort_all(void) {
    static unsigned char running = 0;
    if (running) return;
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
}

void hermes_fatal(void) {
    hermes_device_abort_all();
#if !defined(__AVR__)
    fflush(NULL); /* abort() doesn't flush stdio: keep the trace of what the aborts did */
#endif
    abort();
}

int hermes_device_unregister(hermes_device_t *d) {
    hermes_device_class_t *cls = d->cls;
    if (!cls) return 0;
    hermes_device_t *prev = NULL;
    for (hermes_device_t *it = cls->head; it; prev = it, it = it->next) {
        if (it != d) continue;
        if (prev) prev->next = it->next;
        else cls->head = it->next;
        if (cls->tail == it) cls->tail = prev;
        cls->count--;
        d->next = NULL;
        return 1;
    }
    return 0;
}

int hermes_device_count(void) {
    int n = 0;
    for (hermes_device_class_t *c = g_classes_head; c; c = c->next_class) n += c->count;
    return n;
}

hermes_device_t *hermes_device_at(int i) {
    if (i < 0) return NULL;
    for (hermes_device_class_t *c = g_classes_head; c; c = c->next_class) {
        if (i < (int)c->count) return hermes_device_class_at(c, i);
        i -= (int)c->count;
    }
    return NULL;
}

int hermes_device_count_class(const hermes_device_class_t *cls) {
    return cls->count;
}

hermes_device_t *hermes_device_class_at(const hermes_device_class_t *cls, int i) {
    if (i < 0) return NULL;
    hermes_device_t *it = cls->head;
    while (it && i-- > 0) it = it->next;
    return it;
}
