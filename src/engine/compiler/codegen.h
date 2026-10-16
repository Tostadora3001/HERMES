#ifndef HERMES_CODEGEN_H
#define HERMES_CODEGEN_H

#include "ast.h"

/* Generates C source code for `program` (a NODE_PROGRAM) and returns it as a
 * heap-allocated, null-terminated string. Caller owns the returned buffer.
 * `source_path` is the .hepy file: the generated C carries `#line` directives
 * pointing at it, so gcc/avr-gcc report errors and warnings at the .hepy
 * file and line, not at the generated .c. */
char *codegen_generate(Node *program, const char *source_path);

/* After codegen_generate: did the program call function `name` (a plain
 * call, e.g. a device constructor)? For build flags that depend on use,
 * not only on imports (main.c: -DHERMES_I2C_SLAVE). A call written in an
 * insert_C block isn't seen. */
int codegen_calls(const char *name);
/* The same for methods (x.name(...), any device kind), and whether the
 * program may name a host in x.tcp()/x.send_to() (anything but a literal
 * "a.b.c.d"): main.c's -DHERMES_NET_* flags. */
int codegen_calls_method(const char *name);
int codegen_names_hosts(void);

#endif
