#ifndef HERMES_PARSER_H
#define HERMES_PARSER_H

#include "ast.h"

/* Parses a full .hepy source string into a NODE_PROGRAM node.
 * Exits the process with a diagnostic on syntax errors (acceptable for a
 * v0.1 command-line transpiler). */
Node *parse_program(const char *src, const char *filename);

#endif
