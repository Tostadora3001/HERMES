#include "hermes_device.h"
#include "hermes_sched_internal.h" /* hermes_lock: the lists are shared by every core */
#include <stdlib.h>

#include "hermes_diag.h" /* HERMES_DIAG: stderr on the host, the UART on bare metal */

/* Chain of every kind that has had a device registered, in order of first
 * registration. Zero-initialized (.bss), so registering from an
 * __attribute__((constructor)) -- as hermes_thread.c does for the main
 * thread -- is safe. Devices can be created and destroyed from any core, so
 * every access goes under the framework lock (hermes_lock). */
static hermes_device_class_t *g_classes_head = NULL;
static hermes_device_class_t *g_classes_tail = NULL;

hermes_device_status_t hermes_device_register(hermes_device_t *d,
                                              hermes_device_class_t *cls,
                                              const char *name) {
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
    }

    d->cls = cls;
    d->name = name;
    d->next = NULL;
    if (cls->tail) cls->tail->next = d;
    else cls->head = d;
    cls->tail = d;
    cls->count++;
    hermes_unlock();
    return HERMES_DEVICE_OK;
}

void hermes_device_register_or_abort(hermes_device_t *d,
                                     hermes_device_class_t *cls,
                                     const char *name) {
    hermes_device_status_t st = hermes_device_register(d, cls, name);
    if (st == HERMES_DEVICE_OK) return;
    if (st == HERMES_DEVICE_ERR_NO_ABORT) {
        HERMES_DIAG("hermes_device: kind '%s' doesn't define 'abort' (mandatory: every device "
                    "must be able to be deactivated; see HERMES_DEVICE_CLASS)\n", cls->name);
    } else if (st == HERMES_DEVICE_ERR_CLASS_LIMIT) {
        HERMES_DIAG("hermes_device: limit of kind '%s' reached (at most %d)\n",
                    cls->name, (int)cls->max_instances);
    } else {
        HERMES_DIAG("hermes_device: '%s' was already registered\n", name);
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
