#include "arch.h"
#include <string.h>

const HermesArchProfile HERMES_ARCH_PROFILES[] = {
    { "native", "gcc",     "-pthread",              "hermes_ctx_x86_64.S", ""     },
    { "avr",    "avr-gcc", "-mmcu=atmega328p",      "hermes_ctx_avr.S",    ".elf" },
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
