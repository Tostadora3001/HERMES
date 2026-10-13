/* RISC-V 32-bit (RV32I base, ilp32 ABI) implementation of
 * hermes_ctx_switch/hermes_ctx_make. See hermes_ctx.h for the portable
 * contract.
 *
 * Calling convention (RISC-V psABI, ilp32): arguments in a0..a3; the
 * callee must preserve sp, s0-s11 (s0 is also the frame pointer) and the
 * return address it returns through (ra); everything else (t0-t6, a0-a7)
 * is caller-saved. That is the RISC-V analogue of x86-64's
 * rbx/rbp/r12-r15 in hermes_ctx_x86_64.S and AVR's r2-r17/r28/r29.
 * No floating-point registers are saved: the ilp32 ABI (soft float) has
 * no callee-saved FP registers. A chip with an FPU used through
 * ilp32f/ilp32d would also need fs0-fs11 here.
 *
 * Frame (64 bytes, so sp stays 16-byte aligned as the ABI requires):
 *   0(sp) ra   4(sp) s0   8(sp) s1  ...  48(sp) s11   52..63 padding
 * Written only with RV32I instructions (no compressed, no CSRs), so it
 * fits any RV32 chip: ESP32-C3 (RV32IMC), FE310 (RV32IMAC)...
 */

    .text
    .globl hermes_ctx_switch
    .type hermes_ctx_switch, @function
hermes_ctx_switch:
    /* a0 = from, a1 = to */
    addi sp, sp, -64
    sw   ra,  0(sp)
    sw   s0,  4(sp)
    sw   s1,  8(sp)
    sw   s2, 12(sp)
    sw   s3, 16(sp)
    sw   s4, 20(sp)
    sw   s5, 24(sp)
    sw   s6, 28(sp)
    sw   s7, 32(sp)
    sw   s8, 36(sp)
    sw   s9, 40(sp)
    sw   s10, 44(sp)
    sw   s11, 48(sp)
    sw   sp, 0(a0)          /* from->sp = sp */

    lw   sp, 0(a1)          /* sp = to->sp */
    lw   ra,  0(sp)
    lw   s0,  4(sp)
    lw   s1,  8(sp)
    lw   s2, 12(sp)
    lw   s3, 16(sp)
    lw   s4, 20(sp)
    lw   s5, 24(sp)
    lw   s6, 28(sp)
    lw   s7, 32(sp)
    lw   s8, 36(sp)
    lw   s9, 40(sp)
    lw   s10, 44(sp)
    lw   s11, 48(sp)
    addi sp, sp, 64
    ret                     /* to ra: the caller of hermes_ctx_switch that
                               saved `to`, or `entry` (hermes_ctx_make) */
    .size hermes_ctx_switch, . - hermes_ctx_switch

/* void hermes_ctx_make(hermes_ctx_t *ctx, void *stack_base,
 *                       unsigned long stack_size, void (*entry)(void));
 *
 * Builds on the new stack the frame hermes_ctx_switch pops: ra = entry,
 * s0-s11 = 0. When switched into, `ret` jumps to entry with sp back at
 * the 16-byte-aligned top. entry never returns (it's the thread
 * trampoline), so ra's value inside it doesn't matter.
 */
    .globl hermes_ctx_make
    .type hermes_ctx_make, @function
hermes_ctx_make:
    /* a0 = ctx, a1 = stack_base (top of the stack area), a2 = stack_size
     * (unused), a3 = entry */
    andi t0, a1, -16        /* align the top down to 16 bytes */
    addi t0, t0, -64        /* the frame */
    sw   a3,  0(t0)         /* ra = entry */
    addi t1, t0, 4
    addi t2, t0, 64
1:
    sw   zero, 0(t1)        /* s0..s11 and padding = 0 */
    addi t1, t1, 4
    bltu t1, t2, 1b
    sw   t0, 0(a0)          /* ctx->sp = frame */
    ret
    .size hermes_ctx_make, . - hermes_ctx_make
