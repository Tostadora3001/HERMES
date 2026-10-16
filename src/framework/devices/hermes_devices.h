#ifndef HERMES_DEVICES_H
#define HERMES_DEVICES_H

/* `import hermes_devices` in a .hepy file maps to this header: it pulls in
 * the common device base, the device registry's HERMESPY-facing functions
 * and the built-in device kinds (today only the thread). External devices
 * (gpio, sensors, motors...) are NOT here: each lives in its own module
 * under modules/<name>/ and is pulled in with `import <name>` -- see
 * docs/raw/hermes_modules.en.md and docs/raw/hermes_devices.en.md.
 *
 * Note this does NOT replace `import hermes_threads` -- that header
 * provides the thread API (thread_start, thread_yield, sem_*...) and
 * already includes the thread's `.method()`s (hermes_device_thread.h).
 * This one adds the registry functions and Total_abort().
 */

#include "hermes_device.h"
#include "hermes_device_thread.h"

/* --- registry access for .hepy programs ---------------------------------
 * `device_at(i)` returns a handle (see "handles" in hermes_device.h) of no
 * particular kind: in HERMESPY, `device d = device_at(i)` gets type
 * `hermes_handle_t` (no visible constructor, so no kind -- see
 * devicekind.c), which is what the kind-agnostic `device_name` /
 * `device_kind_name` take -- as does any device's handle (a thread, a
 * gpio...), since every handle has the same type. Out of range: 0 (no
 * device). */
static inline long devices_count(void) { return hermes_device_count(); }
static inline hermes_handle_t device_at(long i) {
    hermes_device_t *d = hermes_device_at((int)i);
    return d ? hermes_device_handle(d) : HERMES_HANDLE_NONE;
}
static inline const char *device_name(hermes_handle_t h) {
    return hermes_device_name(hermes_handle_get(h, NULL, HERMES_PSTR("device"), HERMES_PSTR("device_name")));
}

/* Every device, kind by kind, out like print(): one line per kind (count
 * and memory), one per device (its pins and details), a total line. See
 * hermes_device_dump. */
static inline void devices_dump(void) { hermes_device_dump(); }

/* Bytes taken by every device together (a thread: its whole block; main:
 * 0). The dump gives it per kind. */
static inline long devices_memory(void) { return (long)hermes_device_memory(); }

/* Global, not a method of any device: runs the mandatory abort of every
 * registered device of every kind (outputs LOW, other threads stopped,
 * UART stops receiving...). The calling thread and main keep running. */
static inline void Total_abort(void) { hermes_device_abort_all(); }
static inline const char *device_kind_name(hermes_handle_t h) {
    return hermes_device_class_name(hermes_handle_get(h, NULL, HERMES_PSTR("device"), HERMES_PSTR("device_kind_name")));
}

#endif
