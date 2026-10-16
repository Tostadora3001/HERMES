#include "arch.h"
#include <string.h>

const HermesArchProfile HERMES_ARCH_PROFILES[] = {
    { "native", "gcc",     "-pthread", NULL,        "hermes_ctx_x86_64.S", "",     "uno" },
    /* the chip (-mmcu) and F_CPU come from the board (boards/<board>/board.hboard) */
    { "avr",    "avr-gcc", "-Os -ffunction-sections -fdata-sections -Wl,--gc-sections",      "-mmcu=%s",  "hermes_ctx_avr.S",    ".elf", "uno" },
    /* RISC-V 32-bit microcontrollers, bare metal: picolibc as the C
     * library (approved by the user); the board's mcu is the ISA string
     * (e.g. rv32imc). What is chip-specific (startup, linker script) comes
     * from the board's platform (platform.hplat). */
    { "riscv32", "riscv64-unknown-elf-gcc",
      "--specs=picolibc.specs -Os -ffunction-sections -fdata-sections -Wl,--gc-sections",
      "-march=%s_zicsr -mabi=ilp32", "hermes_ctx_riscv32.S", ".elf", "esp32c3" },
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
