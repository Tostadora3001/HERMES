#ifndef HERMES_MEMPOOL_H
#define HERMES_MEMPOOL_H

/* Fixed registry of "memory profile" imports (`light`/`medium`/`heavy`).
 * Like the architecture markers in arch.h, these are compilation
 * directives, not real libraries: `import light` tells hepyc how much
 * static memory the thread/semaphore runtime should reserve, via a
 * -DHERMES_POOL_BYTES=<N> flag (see hermes_thread.c / docs/raw/hermes_mempool.en.md).
 *
 * Orthogonal to architecture on purpose: an AVR chip can be tiny (ATtiny,
 * ~512 B RAM) or comparatively roomy (ATmega2560, 8 KB), and a memory
 * budget is a property of the chip, not of being "AVR" vs "native". A
 * program can combine any architecture import with at most one memory
 * profile import.
 */
typedef struct {
    const char *import_name;  /* "light" / "medium" / "heavy" */
    long pool_bytes;           /* value substituted into -DHERMES_POOL_BYTES=N */
} HermesMemPoolProfile;

extern const HermesMemPoolProfile HERMES_MEMPOOL_PROFILES[];
extern const int HERMES_MEMPOOL_PROFILE_COUNT;

/* Returns the profile for `import_name`, or NULL if it isn't a recognized
 * memory-profile marker (i.e. it's a normal library import). */
const HermesMemPoolProfile *hermes_mempool_lookup(const char *import_name);

#endif
