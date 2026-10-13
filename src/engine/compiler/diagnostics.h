#ifndef HERMES_DIAGNOSTICS_H
#define HERMES_DIAGNOSTICS_H

/* Runs the C compiler command `cmd` (with LC_ALL=C, so every compiler
 * answers in English) and copies its output to stderr, rewriting the
 * messages it recognizes in HERMESPY terms: e.g.
 *   'y' undeclared (first use in this function)
 *     -> variable 'y' is not declared
 * Together with the #line directives in the generated C (codegen.c) every
 * message already points at the .hepy file and line; this changes the
 * wording. Unknown messages pass through unchanged. With `raw` set, the
 * output is copied as is. Returns the command's exit status (0 = ok). */
int hermes_run_compiler(const char *cmd, int raw);

/* The rewrite of one line of compiler output (for tests). Returns a
 * malloc'd string. */
char *hermes_translate_diagnostic(const char *line);

#endif
