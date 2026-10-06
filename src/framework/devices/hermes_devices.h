#ifndef HERMES_DEVICES_H
#define HERMES_DEVICES_H

/* `import hermes_devices` in a .hepy file maps to this header: it pulls in
 * the common device base plus every concrete device kind, internal and
 * external. See docs/raw/hermes_devices.en.md.
 *
 * Note this does NOT replace `import hermes_threads` -- that header still
 * provides the automatic/concrete-tier thread API (thread_start,
 * threads_run, thread_yield, sem_*...). This one adds the `.method()`
 * dot-call surface (device_thread_join, device_gpio_write, ...) that the
 * transpiler resolves `receiver.method(args)` calls to. A program using
 * both the plain function style and the dot-call style imports both.
 */

#include "hermes_device.h"
#include "hermes_device_thread.h"
#include "hermes_device_gpio.h"

#endif
