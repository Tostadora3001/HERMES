#ifndef HERMES_CODEGEN_H
#define HERMES_CODEGEN_H

#include "ast.h"

/* Generates C source code for `program` (a NODE_PROGRAM) and returns it as a
 * heap-allocated, null-terminated string. Caller owns the returned buffer.
 * `source_path` is the .hepy file: the generated C carries `#line` directives
 * pointing at it, so gcc/avr-gcc report errors and warnings at the .hepy
 * file and line, not at the generated .c. */
char *codegen_generate(Node *program, const char *source_path);

#endif
