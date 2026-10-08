#include "arch.h"
#include <string.h>

const HermesArchProfile HERMES_ARCH_PROFILES[] = {
    { "native", "gcc",     "-pthread",              "hermes_ctx_x86_64.S", ""     },
    /* F_CPU: the Uno's 16 MHz crystal -- the timer and UART drivers
     * derive their register values from it (src/framework/drivers). */
    { "avr",    "avr-gcc", "-mmcu=atmega328p -DF_CPU=16000000UL -Os", "hermes_ctx_avr.S", ".elf" },
};
const int HERMES_ARCH_PROFILE_COUNT = sizeof(HERMES_ARCH_PROFILES) / sizeof(HERMES_ARCH_PROFILES[0]);

const HermesArchProfile *hermes_arch_lookup(const char *import_name) {
    for (int i = 0; i < HERMES_ARCH_PROFILE_COUNT; i++) {
        if (strcmp(HERMES_ARCH_PROFILES[i].import_name, import_name) == 0) {
            return &HERMES_ARCH_PROFILES[i];
        }
    }
    return NULL;
}
