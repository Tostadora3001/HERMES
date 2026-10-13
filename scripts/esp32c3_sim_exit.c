/* Solo para las pruebas (como avr_sim_exit.c): en la ESP32-C3 emulada por
 * QEMU, cuando el programa termina (main() vuelve, exit() o un error
 * fatal), espera a que salga el ultimo byte por la UART y termina QEMU por
 * semihosting (SYS_EXIT; QEMU necesita -semihosting-config enable=on).
 * Sustituye al _exit debil de la plataforma, que se queda parado como
 * haria un microcontrolador real. No forma parte del framework. */
#include <stdint.h>

#define UART_STATUS_REG (*(volatile uint32_t *)0x6000001Cu)

__attribute__((noreturn)) void _exit(int status) {
    while ((UART_STATUS_REG >> 16) & 0x3FFu) {} /* TX FIFO empty */
    /* SYS_EXIT (0x18); on RV32 the parameter is the reason:
     * ADP_Stopped_ApplicationExit (0x20026) = exit code 0, anything else 1 */
    register long a0 __asm__("a0") = 0x18;
    register long a1 __asm__("a1") = status == 0 ? 0x20026 : 0x20023;
    __asm__ __volatile__(
        ".option push\n"
        ".option norvc\n"
        ".balign 16\n"
        "slli x0, x0, 0x1f\n"   /* the semihosting call sequence */
        "ebreak\n"
        "srai x0, x0, 7\n"
        ".option pop\n"
        : "+r"(a0) : "r"(a1) : "memory");
    for (;;) {}
}
