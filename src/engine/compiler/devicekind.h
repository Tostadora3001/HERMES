#ifndef HERMES_DEVICEKIND_H
#define HERMES_DEVICEKIND_H

/* Registry mapping a recognized "device constructor" function name (the
 * right-hand side of `device x = ctor(...)`) to the device's kind and the
 * concrete C handle type that should replace the generic HERMESPY `device`
 * type for that variable. See codegen.c's device-kind tracking comment and
 * docs/raw/hermes_devices.en.md.
 *
 * Two sources of entries:
 *   - built-in kinds (framework devices, e.g. the thread), fixed in
 *     devicekind.c;
 *   - kinds declared by imported modules (`device = ...` lines in
 *     modules/<name>/module.hmod), added by module.c via
 *     hermes_devicekind_register before code generation starts.
 *
 * `kind` doubles as the middle part of the naming convention every device
 * method follows: `receiver.method(args)` -> `device_<kind>_method(receiver, args)`.
 * No per-method table is needed -- it's a plain string substitution, not a
 * lookup, so adding a new method to an existing device kind needs no
 * change here, only a new `device_<kind>_<method>` function.
 */
typedef struct {
    const char *constructor_name;
    const char *kind;
    const char *c_type;
} HermesDeviceKind;

#define HERMES_MAX_MODULE_DEVICE_KINDS 64

/* Returns the entry for `constructor_name`, or NULL if it isn't a
 * recognized device constructor. */
const HermesDeviceKind *hermes_devicekind_lookup_constructor(const char *constructor_name);

/* Adds a module-declared constructor. Strings are not copied (callers pass
 * strings that live for the whole run). Returns 0 (with a diagnostic on
 * stderr) if the constructor name is already taken or the table is full. */
int hermes_devicekind_register(const char *constructor_name, const char *kind, const char *c_type);

#endif
