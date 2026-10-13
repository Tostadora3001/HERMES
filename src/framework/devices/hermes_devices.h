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
 * `device_at(i)` returns the generic base pointer: in HERMESPY,
 * `device d = device_at(i)` gets type `hermes_device_t *` (no visible
 * constructor, so no kind -- see devicekind.c), which is exactly what the
 * kind-agnostic `device_name`/`device_kind_name` need. Those two are macros
 * so they also accept a concrete handle (a thread, a gpio...) directly. */
static inline long devices_count(void) { return hermes_device_count(); }
static inline hermes_device_t *device_at(long i) { return hermes_device_at((int)i); }
#define device_name(d) hermes_device_name((const hermes_device_t *)(d))

/* Global, not a method of any device: runs the mandatory abort of every
 * registered device of every kind (outputs LOW, other threads stopped,
 * UART stops receiving...). The calling thread and main keep running. */
static inline void Total_abort(void) { hermes_device_abort_all(); }
#define device_kind_name(d) hermes_device_class_name((const hermes_device_t *)(d))

#endif
