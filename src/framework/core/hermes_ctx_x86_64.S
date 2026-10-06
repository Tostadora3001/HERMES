/* x86-64 (System V ABI) implementation of hermes_ctx_switch/hermes_ctx_make.
 * See hermes_ctx.h for the portable contract these two symbols must satisfy.
 *
 * hermes_ctx_switch(from, to):
 *   Saves the six callee-saved GPRs onto the current stack, stores the
 *   resulting rsp into *from, loads rsp from *to, restores the six
 *   callee-saved GPRs from the new stack, and `ret`s into whatever address
 *   sits on top of the new stack. That return address is either:
 *     - a real return address, if `to` was previously saved by a call to
 *       hermes_ctx_switch itself, or
 *     - the trampoline address installed by hermes_ctx_make, the first time
 *       a freshly created thread is switched into.
 */

    .text
    .globl hermes_ctx_switch
    .type hermes_ctx_switch, @function
hermes_ctx_switch:
    /* rdi = from, rsi = to */
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, (%rdi)
    movq (%rsi), %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
    .size hermes_ctx_switch, . - hermes_ctx_switch

/* void hermes_ctx_make(hermes_ctx_t *ctx, void *stack_base,
 *                       unsigned long stack_size, void (*entry)(void));
 *
 * Builds, on the new thread's own stack, the exact frame that
 * hermes_ctx_switch expects to pop when it later switches into this
 * context: six saved-register slots (values irrelevant for a brand new
 * thread) followed by a return address pointing at `entry`.
 */
    .globl hermes_ctx_make
    .type hermes_ctx_make, @function
hermes_ctx_make:
    /* rdi = ctx, rsi = stack_base (already the top of the stack region),
     * rdx = stack_size (unused here, kept for symmetry/future bounds
     * checks), rcx = entry */
    movq %rsi, %rax
    andq $-16, %rax             /* align down to 16 bytes */

    /* 8 bytes of padding so the final frame size below (64 bytes) keeps
     * %rax 16-byte aligned, matching the ABI's expectation that %rsp is
     * 16-aligned right before a `call` (here simulated by `ret`). */
    subq $8, %rax

    subq $8, %rax                /* slot for the "return address" */
    movq %rcx, (%rax)            /* -> entry */

    subq $8, %rax; movq $0, (%rax) /* rbx */
    subq $8, %rax; movq $0, (%rax) /* rbp */
    subq $8, %rax; movq $0, (%rax) /* r12 */
    subq $8, %rax; movq $0, (%rax) /* r13 */
    subq $8, %rax; movq $0, (%rax) /* r14 */
    subq $8, %rax; movq $0, (%rax) /* r15 */

    movq %rax, (%rdi)            /* ctx->sp = rax */
    ret
    .size hermes_ctx_make, . - hermes_ctx_make

    .section .note.GNU-stack,"",@progbits
