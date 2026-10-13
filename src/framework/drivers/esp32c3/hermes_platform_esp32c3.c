/* ESP32-C3 platform startup, after hermes_start_esp32c3.S: what a C
 * program expects before main(), with no ESP-IDF and no second-stage
 * bootloader. See esp32c3.ld for the memory layout. */
#include <stdint.h>
#include <string.h>
#include "hermes_esp32c3_regs.h"

int main(void);

extern char __data_start[], __data_end[], __data_lma[];
extern char __tls_base[], __tdata_end[], __tdata_lma[];
extern char __tbss_start[], __tbss_end[], __bss_start[], __bss_end[];
typedef void (*hermes_init_fn)(void);
extern hermes_init_fn __preinit_array_start[], __preinit_array_end[];
extern hermes_init_fn __init_array_start[], __init_array_end[];
extern hermes_init_fn __fini_array_start[], __fini_array_end[];

/* IROM and DROM map the same flash offset: an IROM address read as data
 * goes through DROM. */
#define IROM_TO_DROM(p) ((const void *)((uintptr_t)(p) - 0x42000000u + 0x3C000000u))

/* The ROM leaves the watchdogs that guard the boot armed: without a
 * second-stage bootloader nobody disarms them, and the chip would reset
 * a few seconds into the program. HERMES doesn't feed watchdogs (yet):
 * all of them off. */
static void watchdogs_off(void) {
    static const uint32_t timg[2] = { TIMG0_BASE, TIMG1_BASE };
    for (int i = 0; i < 2; i++) {
        HERMES_REG(TIMG_WDTWPROTECT_REG(timg[i])) = HERMES_WDT_WKEY;
        HERMES_REG(TIMG_WDTCONFIG0_REG(timg[i])) = TIMG_WDT_CONF_UPDATE_EN; /* EN = 0 */
        HERMES_REG(TIMG_WDTWPROTECT_REG(timg[i])) = 0;
    }
    HERMES_REG(RTC_CNTL_WDTWPROTECT_REG) = HERMES_WDT_WKEY;
    HERMES_REG(RTC_CNTL_WDTCONFIG0_REG) = 0;
    HERMES_REG(RTC_CNTL_WDTWPROTECT_REG) = 0;
    /* the super watchdog can't be turned off: let the hardware feed it */
    HERMES_REG(RTC_CNTL_SWD_WPROTECT_REG) = HERMES_SWD_WKEY;
    HERMES_REG(RTC_CNTL_SWD_CONF_REG) |= RTC_CNTL_SWD_AUTO_FEED_EN;
    HERMES_REG(RTC_CNTL_SWD_WPROTECT_REG) = 0;
}

/* Where a program stops: after main() returns (a microcontroller program
 * isn't meant to), on exit() and on abort()/hermes_fatal(). Weak: the test
 * suite replaces it to end the emulator (scripts/esp32c3_sim_exit.c). */
__attribute__((weak, noreturn)) void _exit(int status) {
    (void)status;
    __asm__ __volatile__("csrci mstatus, 8");
    for (;;) __asm__ __volatile__("wfi");
}

__attribute__((noreturn)) void hermes_esp32c3_start(void) {
    watchdogs_off();
    memcpy(__data_start, IROM_TO_DROM(__data_lma), (size_t)(__data_end - __data_start));
    memcpy(__tls_base, IROM_TO_DROM(__tdata_lma), (size_t)(__tdata_end - __tls_base));
    memset(__tbss_start, 0, (size_t)(__tbss_end - __tbss_start));
    memset(__bss_start, 0, (size_t)(__bss_end - __bss_start));
    for (hermes_init_fn *f = __preinit_array_start; f < __preinit_array_end; f++) (*f)();
    for (hermes_init_fn *f = __init_array_start; f < __init_array_end; f++) (*f)();
    int status = main();
    for (hermes_init_fn *f = __fini_array_start; f < __fini_array_end; f++) (*f)();
    _exit(status);
}
