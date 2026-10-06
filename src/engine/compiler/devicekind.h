#ifndef HERMES_DEVICEKIND_H
#define HERMES_DEVICEKIND_H

/* Fixed registry mapping a recognized "device constructor" function name
 * (the right-hand side of `device x = ctor(...)`) to the device's kind and
 * the concrete C handle type that should replace the generic HERMESPY
 * `device` type for that variable. See codegen.c's device-kind tracking
 * comment and docs/raw/hermes_devices.en.md.
 *
 * `kind` doubles as the middle part of the naming convention every device
 * method follows: `receiver.method(args)` -> `device_<kind>_method(receiver, args)`.
 * No per-method table is needed -- it's a plain string substitution, not a
 * lookup, so adding a new method to an existing device kind needs no
 * change here, only a new `device_<kind>_<method>` function in
 * src/framework/devices.
 */
typedef struct {
    const char *constructor_name;
    const char *kind;
    const char *c_type;
} HermesDeviceKind;

extern const HermesDeviceKind HERMES_DEVICE_KINDS[];
extern const int HERMES_DEVICE_KIND_COUNT;

/* Returns the entry for `constructor_name`, or NULL if it isn't a
 * recognized device constructor. */
const HermesDeviceKind *hermes_devicekind_lookup_constructor(const char *constructor_name);

#endif
