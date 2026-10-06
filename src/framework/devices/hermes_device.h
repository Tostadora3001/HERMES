#ifndef HERMES_DEVICE_H
#define HERMES_DEVICE_H

/* Common base for every "device" in HERMES: the abstraction, per the
 * original specification (docs/raw/Definicion.es.md, "Gestion de
 * recursos"), for anything the system manages as an object with its own
 * data and methods -- external hardware (sensors, actuators, other
 * microcontrollers) as well as internal resources (threads, in the
 * future maybe other runtime primitives).
 *
 * Every concrete device struct embeds this as its FIRST member:
 *
 *   typedef struct {
 *       hermes_device_t device;   // must be first
 *       ... kind-specific fields ...
 *   } hermes_device_whatever_t;
 *
 * That makes a `hermes_device_whatever_t *` safely reinterpretable as a
 * `hermes_device_t *` (the struct's address IS its base member's address,
 * same idiom used throughout C for this -- e.g. BSD's sockaddr/sockaddr_in).
 * This base is deliberately NOT a vtable: HERMESPY's `receiver.method(args)`
 * syntax is resolved entirely at transpile time, to a plain function call
 * (`device_<kind>_<method>(receiver, args)`), based on the receiver's
 * statically-known kind -- see src/engine/compiler/devicekind.c and
 * docs/raw/hermes_devices.en.md. This base exists only for the handful of
 * operations that genuinely apply to *any* device regardless of kind
 * (`device_kind`, `device_name`), e.g. for a future generic device
 * registry/listing. A `device`-typed function parameter (whose concrete
 * kind the transpiler can't know) can use those, but not a kind-specific
 * `.method()` -- that is a real, intentional limitation of this first
 * version, not an oversight.
 */
typedef enum {
    HERMES_DEVICE_THREAD, /* internal: src/framework/devices/hermes_device_thread.h */
    HERMES_DEVICE_GPIO,   /* external: src/framework/devices/hermes_device_gpio.h */
} hermes_device_kind_t;

typedef struct {
    hermes_device_kind_t kind;
    const char *name;
} hermes_device_t;

static inline hermes_device_kind_t hermes_device_kind(const hermes_device_t *d) {
    return d->kind;
}

static inline const char *hermes_device_name(const hermes_device_t *d) {
    return d->name;
}

#endif
