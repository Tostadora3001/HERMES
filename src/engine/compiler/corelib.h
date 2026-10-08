#ifndef HERMES_CORELIB_H
#define HERMES_CORELIB_H

/* Fixed table of what each framework (core) library needs linked, so a
 * .hepy never has to list framework sources by hand: `import
 * hermes_threads` is enough for hepyc to add hermes_thread.c, hermes_sem.c,
 * hermes_device.c and the architecture's context-switch backend.
 * Same "reglas fijas" style as arch.c / mempool.c.
 *
 * Two kinds of entries:
 *   - importable libraries, named like the import (`hermes_threads`,
 *     `hermes_uart`, ...);
 *   - internal groups, named with a prefix no import can spell:
 *     `@runtime` (scheduler + semaphores + device registry) and
 *     `driver:<name>` (one per driver in src/framework/drivers, which is
 *     also how a module's `drivers = ...` key refers to them).
 * Header-only libraries (hermes_stdlib, hermes_parallel) need no entry. */

#define HERMES_CORELIB_MAX_SOURCES 4
#define HERMES_CORELIB_MAX_REQUIRES 4

typedef struct {
    const char *name;
    const char *sources[HERMES_CORELIB_MAX_SOURCES];   /* relative to <repo>/src; NULL-terminated */
    const char *requires[HERMES_CORELIB_MAX_REQUIRES]; /* other entries; NULL-terminated */
    int needs_ctx; /* 1: link the target architecture's hermes_ctx_*.S too */
} HermesCoreLib;

const HermesCoreLib *hermes_corelib_lookup(const char *name);

/* Marks `name` (and, recursively, what it requires) as used. Returns 0 if
 * `name` isn't in the table. */
int hermes_corelib_select(const char *name);

int hermes_corelib_selected_count(void);
const HermesCoreLib *hermes_corelib_selected_at(int i); /* dependencies first */

/* 1 if any selected library needs the context-switch backend. */
int hermes_corelib_needs_ctx(void);

#endif
