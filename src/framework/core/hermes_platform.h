#ifndef HERMES_PLATFORM_H
#define HERMES_PLATFORM_H

/* What kind of machine the framework is compiled for. Only macros: safe
 * to include before anything else (even before feature-test macros).
 *
 * HERMES_BARE_METAL = 1: a microcontroller with no operating system and
 *   one core -- AVR, and RISC-V without Linux (ESP32-C3...). Plain globals
 *   instead of thread-local storage, interrupts masked as the framework
 *   lock, main's stack bounded by a canary, no stdio: output only through
 *   hermes_putchar (the UART driver).
 * HERMES_BARE_METAL = 0: the host (Linux): pthreads (one per core), stdio.
 *
 * What differs between bare-metal chips beyond that (masking interrupts,
 * where main's stack ends) is chosen by ISA in hermes_thread.c. */
#if defined(__AVR__) || (defined(__riscv) && !defined(__linux__))
#define HERMES_BARE_METAL 1
#else
#define HERMES_BARE_METAL 0
#endif

#endif
