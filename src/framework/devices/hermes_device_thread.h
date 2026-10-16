#ifndef HERMES_DEVICE_THREAD_H
#define HERMES_DEVICE_THREAD_H

/* The thread as HERMES's first *internal* device. hermes_thread_t (in
 * src/framework/core/hermes_thread.h) already embeds hermes_device_t as
 * its first member, so a hermes_thread_t* already IS a hermes_device_t* --
 * nothing to wrap there. What's here are the device *methods*: thin,
 * header-only wrappers with the `device_thread_<method>` names the
 * transpiler emits for `t.method(args)` (see
 * src/engine/compiler/devicekind.c and docs/raw/hermes_devices.en.md),
 * calling straight into hermes_thread.c -- no logic here.
 *
 * Not every hermes_thread_t operation becomes a device method: only ones
 * that act on a *specific* instance do. `hermes_thread_yield`/
 * `hermes_thread_current` apply to "whichever thread is currently running",
 * not to a particular receiver, so they stay plain functions (exposed as
 * `thread_yield`/`thread_self_id` in hermes_threads.h) rather than
 * `t.yield()`-style methods.
 */

#include <stdbool.h>
#include "hermes_device.h"
#include "hermes_thread.h"

/* t.run(): run on this core until it yields, blocks or finishes */
static inline void device_thread_run(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "run");
    hermes_thread_run(t);
}

/* t.launch() / t.launch_on(n): run on another core */
static inline void device_thread_launch(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "launch");
    hermes_thread_launch(t);
}
static inline void device_thread_launch_on(hermes_thread_handle_t h_t, long core) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "launch_on");
    hermes_thread_launch_on(t, (int)core);
}

/* t.wait(): wait (running nothing) until it finishes. `join` is the old
 * name, kept as an alias. */
static inline void device_thread_wait(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "wait");
    hermes_thread_wait(t);
}
static inline void device_thread_join(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "join");
    hermes_thread_wait(t);
}

static inline bool device_thread_done(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "done");
    return hermes_thread_done(t) != 0;
}

static inline void device_thread_pause(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "pause");
    hermes_thread_pause(t);
}
static inline void device_thread_resume(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "resume");
    hermes_thread_resume(t);
}

/* t.destroy(): abort and release its memory; `t` can't be used afterwards */
static inline void device_thread_destroy(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "destroy");
    hermes_thread_destroy(t);
}

/* Mandatory device abort (see hermes_device.h): `t.abort()`. */
static inline void device_thread_abort(hermes_thread_handle_t h_t) {
    hermes_thread_t *t = HERMES_THREAD_GET(h_t, "abort");
    hermes_thread_abort(t);
}

#endif
