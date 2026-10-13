/* ESP32-C3: first instructions of a HERMES program (direct boot, see
 * esp32c3.ld). Flash offset 0 holds the two magic words that make the ROM
 * boot the flash directly; it jumps to the code right after them
 * (0x42000008). Then: gp, sp and tp, and the rest in C
 * (hermes_esp32c3_start, hermes_platform_esp32c3.c). */

    .section .text.entry, "ax"
    .globl _start
    .word 0xaedb041d
    .word 0xaedb041d
_start:
    .option push
    .option norelax
    la   gp, __global_pointer$   /* without relaxation: gp isn't set yet */
    .option pop
    la   sp, __stack_top         /* main's stack: the top of RAM */
    la   tp, __tls_base          /* picolibc's thread-local data (errno...) */
    csrci mstatus, 8             /* interrupts off until something enables them */
    call hermes_esp32c3_start    /* never returns */
1:  j    1b
