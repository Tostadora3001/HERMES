/* AVR implementation of hermes_ctx_switch/hermes_ctx_make. See hermes_ctx.h
 * for the portable contract. Calling convention (avr-gcc default ABI,
 * verified empirically against avr-gcc 7.3.0 for atmega328p -- arguments
 * are packed into registers starting at r25 and descending):
 *
 *   hermes_ctx_switch(hermes_ctx_t *from, hermes_ctx_t *to):
 *     from = r25:r24, to = r23:r22
 *   hermes_ctx_make(hermes_ctx_t *ctx, void *stack_base,
 *                    unsigned long stack_size, void (*entry)(void)):
 *     ctx = r25:r24, stack_base = r23:r22, stack_size = r21:r20:r19:r18
 *     (LSB in r18), entry = r17:r16
 *
 * Call-saved (callee must preserve) registers on AVR: r2-r17, r28, r29.
 * Everything else (r0, r18-r27, r30, r31, SREG) is call-clobbered, same
 * tier as a scratch register -- that is the AVR analogue of x86-64's
 * callee-saved rbx/rbp/r12-r15 in hermes_ctx_x86_64.S.
 */

#define SPL_IO 0x3d
#define SPH_IO 0x3e
#define SREG_IO 0x3f

    .text
    .global hermes_ctx_switch
    .type hermes_ctx_switch, @function
hermes_ctx_switch:
    push r2
    push r3
    push r4
    push r5
    push r6
    push r7
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push r16
    push r17
    push r28
    push r29

    in   r0, SPL_IO
    movw r26, r24        /* X = from */
    st   X+, r0
    in   r0, SPH_IO
    st   X,  r0

    movw r26, r22        /* X = to */
    ld   r24, X+
    ld   r25, X          /* r25:r24 = to->sp */

    in   r0, SREG_IO
    cli
    out  SPH_IO, r25
    out  SREG_IO, r0
    out  SPL_IO, r24

    pop  r29
    pop  r28
    pop  r17
    pop  r16
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  r7
    pop  r6
    pop  r5
    pop  r4
    pop  r3
    pop  r2
    ret
    .size hermes_ctx_switch, . - hermes_ctx_switch

    .global hermes_ctx_make
    .type hermes_ctx_make, @function
hermes_ctx_make:
    /* AVR PUSH stores at the current SP and THEN decrements it; POP
     * increments SP FIRST and then reads -- so a value pushed while SP was
     * S ends up living at address S, and after the push SP becomes S-1. The
     * practical consequence: relative to final_sp (the value we hand to
     * hermes_ctx_switch, i.e. what SP will be once this fake frame is
     * "current"), the first byte hermes_ctx_switch's restore path will
     * actually read is at final_sp+1, NOT final_sp+0 (verified empirically
     * against avr-gcc/simavr -- see commit history / docs/raw/hermes_threads.en.md).
     * Likewise CALL pushes the return address as [PCH at the lower address,
     * PCL at the higher address] -- the reverse of normal AVR little-endian
     * data layout.
     *
     * Layout relative to final_sp (adiw/sbiw only work on r24/r26/r28/r30,
     * so the arithmetic happens in X and is stashed in r18:r19 since X gets
     * reused below):
     *   final_sp+1  .. final_sp+18 : the 18 saved-register slots popped by
     *                                 hermes_ctx_switch (r29,r28,r17..r2)
     *   final_sp+19                : entry address HIGH byte (PCH)
     *   final_sp+20                : entry address LOW byte  (PCL)
     */
    movw r26, r22         /* X = stack_base */
    sbiw r26, 21          /* X = final_sp = stack_base - 21 */
    movw r18, r26         /* r18:r19 = final_sp */

    adiw r26, 19          /* X = final_sp + 19 */
    st   X+, r17          /* final_sp+19 = entry HIGH byte */
    st   X,  r16          /* final_sp+20 = entry LOW byte */

    movw r26, r18         /* X = final_sp */
    adiw r26, 1           /* X = final_sp + 1 (first saved-register slot) */
    clr  r0
    ldi  r20, 18
1:
    st   X+, r0           /* zero the 18 saved-register slots (value irrelevant) */
    dec  r20
    brne 1b

    movw r26, r24         /* X = ctx (points at ctx->sp) */
    st   X+, r18
    st   X,  r19
    ret
    .size hermes_ctx_make, . - hermes_ctx_make
