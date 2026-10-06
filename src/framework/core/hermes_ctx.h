#ifndef HERMES_CTX_H
#define HERMES_CTX_H

/* Minimal CPU-context primitive used to build the thread scheduler on top.
 * This is the ONLY architecture-specific piece of the threading runtime —
 * everything else in src/framework/core is portable C. To port the
 * scheduler to a new target (e.g. AVR), implement `hermes_ctx_switch` for
 * that architecture's calling convention; the rest of the runtime does not
 * need to change. */

typedef struct {
    void *sp; /* saved stack pointer; everything else lives on the stack */
} hermes_ctx_t;

/* Saves the currently running context into `from`, then restores `to` and
 * resumes execution there. When some other context later switches back into
 * `from`, this function returns normally to its caller. */
void hermes_ctx_switch(hermes_ctx_t *from, hermes_ctx_t *to);

/* Prepares `ctx` so that the first time it is switched into (via
 * hermes_ctx_switch), execution begins at `entry()` running on top of the
 * stack region [stack_base - stack_size, stack_base). `stack_base` is the
 * highest address of the stack area (the stack grows down from there). */
void hermes_ctx_make(hermes_ctx_t *ctx, void *stack_base, unsigned long stack_size, void (*entry)(void));

#endif
