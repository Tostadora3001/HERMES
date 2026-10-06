#include "devicekind.h"
#include <string.h>

const HermesDeviceKind HERMES_DEVICE_KINDS[] = {
    /* internal device: a hermes_threads.h thread IS a device (see
     * src/framework/devices/hermes_device_thread.h) -- same constructors,
     * same handle type, just also reachable via `.join()` etc. */
    { "thread_start",    "thread", "hermes_thread_handle_t" },
    { "thread_start_ex", "thread", "hermes_thread_handle_t" },

    /* external device: a single digital output pin
     * (src/framework/devices/hermes_device_gpio.h). */
    { "gpio", "gpio", "hermes_device_gpio_handle_t" },
};
const int HERMES_DEVICE_KIND_COUNT =
    sizeof(HERMES_DEVICE_KINDS) / sizeof(HERMES_DEVICE_KINDS[0]);

const HermesDeviceKind *hermes_devicekind_lookup_constructor(const char *constructor_name) {
    for (int i = 0; i < HERMES_DEVICE_KIND_COUNT; i++) {
        if (strcmp(HERMES_DEVICE_KINDS[i].constructor_name, constructor_name) == 0) {
            return &HERMES_DEVICE_KINDS[i];
        }
    }
    return NULL;
}
