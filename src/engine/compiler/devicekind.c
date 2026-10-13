#include "devicekind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Built-in kinds: the ones that live in src/framework (not in a module). */
static const HermesDeviceKind HERMES_BUILTIN_DEVICE_KINDS[] = {
    /* internal device: a hermes_threads.h thread IS a device (see
     * src/framework/devices/hermes_device_thread.h) -- same constructors,
     * same handle type, just also reachable via `.join()` etc. */
    { "thread_start",    "thread", "hermes_thread_handle_t" },
    { "thread_start_ex", "thread", "hermes_thread_handle_t" },

    /* core device over the UART driver (src/framework/devices/hermes_uart.h,
     * `import hermes_uart`): communication buses are core, not modules. */
    { "uart", "uart", "hermes_device_uart_handle_t" },
};
#define HERMES_BUILTIN_DEVICE_KIND_COUNT \
    ((int)(sizeof(HERMES_BUILTIN_DEVICE_KINDS) / sizeof(HERMES_BUILTIN_DEVICE_KINDS[0])))

/* Kinds added at transpile time by imported modules (module.c). */
static HermesDeviceKind g_module_kinds[HERMES_MAX_MODULE_DEVICE_KINDS];
static int g_module_kind_count = 0;

const HermesDeviceKind *hermes_devicekind_lookup_constructor(const char *constructor_name) {
    for (int i = 0; i < HERMES_BUILTIN_DEVICE_KIND_COUNT; i++) {
        if (strcmp(HERMES_BUILTIN_DEVICE_KINDS[i].constructor_name, constructor_name) == 0) {
            return &HERMES_BUILTIN_DEVICE_KINDS[i];
        }
    }
    for (int i = 0; i < g_module_kind_count; i++) {
        if (strcmp(g_module_kinds[i].constructor_name, constructor_name) == 0) {
            return &g_module_kinds[i];
        }
    }
    return NULL;
}

int hermes_devicekind_register(const char *constructor_name, const char *kind, const char *c_type) {
    if (hermes_devicekind_lookup_constructor(constructor_name)) {
        fprintf(stderr, "hepyc: device constructor '%s' is already defined\n", constructor_name);
        return 0;
    }
    if (g_module_kind_count >= HERMES_MAX_MODULE_DEVICE_KINDS) {
        fprintf(stderr, "hepyc: too many device constructors in modules (at most %d)\n",
                HERMES_MAX_MODULE_DEVICE_KINDS);
        return 0;
    }
    HermesDeviceKind *k = &g_module_kinds[g_module_kind_count++];
    k->constructor_name = constructor_name;
    k->kind = kind;
    k->c_type = c_type;
    return 1;
}
