#ifndef HERMES_DEVICE_THREAD_H
#define HERMES_DEVICE_THREAD_H

/* The thread as HERMES's first *internal* device. hermes_thread_t (in
 * src/framework/core/hermes_thread.h) already embeds hermes_device_t as
 * its first member, so a hermes_thread_t* already IS a hermes_device_t* --
 * nothing to wrap there. What's here are the device *methods*: thin,
 * header-only wrappers with the `device_thread_<method>` names the
 * transpiler emits for `t.method(args)` (see
 * src/engine/compiler/devicekind.c and docs/raw/hermes_devices.en.md),
 * calling straight into the existing, unchanged hermes_thread.c
 * implementation -- no logic duplicated or rewritten here.
 *
 * Not every hermes_thread_t operation becomes a device method: only ones
 * that act on a *specific* instance do (`join`). `hermes_thread_yield`/
 * `hermes_thread_current` apply to "whichever thread is currently running",
 * not to a particular receiver, so they stay plain functions (exposed as
 * `thread_yield`/`thread_self_id` in hermes_threads.h) rather than
 * `t.yield()`-style methods.
 */

#include "hermes_device.h"
#include "hermes_thread.h"

static inline void device_thread_join(hermes_thread_t *t) {
    hermes_thread_join(t);
}

/* Mandatory device abort (see hermes_device.h): `t.abort()`. */
static inline void device_thread_abort(hermes_thread_t *t) {
    hermes_thread_abort(t);
}

#endif
