#ifndef HERMES_CODEGEN_H
#define HERMES_CODEGEN_H

#include "ast.h"

/* Generates C source code for `program` (a NODE_PROGRAM) and returns it as a
 * heap-allocated, null-terminated string. Caller owns the returned buffer. */
char *codegen_generate(Node *program);

#endif
