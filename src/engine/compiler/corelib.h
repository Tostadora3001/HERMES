#ifndef HERMES_CORELIB_H
#define HERMES_CORELIB_H

/* Fixed table of what each framework (core) library needs linked, so a
 * .hepy never has to list framework sources by hand: `import
 * hermes_threads` is enough for hepyc to add hermes_thread.c, hermes_sem.c,
 * hermes_device.c and the architecture's context-switch backend.
 * Same "reglas fijas" style as arch.c / threadprofile.c.
 *
 * Two kinds of entries:
 *   - importable libraries, named like the import (`hermes_threads`,
 *     `hermes_uart`, ...);
 *   - internal groups, named with a prefix no import can spell:
 *     `@runtime` (thread library + semaphores + device registry) and
 *     `driver:<name>` (one per driver in src/framework/drivers, which is
 *     also how a module's `drivers = ...` key refers to them).
 * Header-only libraries (hermes_stdlib) need no entry.
 *
 * A source may contain `{platform}`: it is replaced by the target's
 * platform (board.h: the chip family -- avr, esp32c3, native), e.g. a
 * driver's implementation
 * "framework/drivers/{platform}/hermes_drv_gpio_{platform}.c". */

#define HERMES_CORELIB_MAX_SOURCES 6
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
int hermes_corelib_is_selected(const char *name);
const HermesCoreLib *hermes_corelib_selected_at(int i); /* dependencies first */

/* Checks the imports that must be written together by the user (e.g.
 * hermes_os needs hermes_threads and a scheduler). Prints every violation
 * and returns 0 if there is any. */
int hermes_corelib_check_rules(const char *const *imports, int n);

/* `source` with every `{platform}` replaced by `platform` (malloc'd). */
char *hermes_corelib_source_for_platform(const char *source, const char *platform);

/* 1 if any selected library needs the context-switch backend. */
int hermes_corelib_needs_ctx(void);

#endif
