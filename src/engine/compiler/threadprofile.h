#ifndef HERMES_THREADPROFILE_H
#define HERMES_THREADPROFILE_H

/* Fixed registry of the thread-size profile imports (`light`/`medium`/
 * `heavy`). Like the architecture markers in arch.h, these are compilation
 * directives, not real libraries: `import light` tells hepyc the default
 * size of each thread's block (heap + stack). hepyc passes only the level
 * (-DHERMES_THREAD_PROFILE=1/2/3); hermes_thread.h turns it into sizes per
 * target, since the same .c is compiled for several targets and the host
 * needs much bigger stacks (glibc's printf alone wants ~2 KB). It does NOT
 * limit the program's total memory: blocks come from whatever is free.
 * See docs/raw/hermes_thread_profiles.en.md. At most one profile per program;
 * without one, medium. (Until 2026-10-09 they were a static memory pool,
 * in files named mempool.{c,h}.)
 */
typedef struct {
    const char *import_name;  /* "light" / "medium" / "heavy" */
    int level;                /* -> -DHERMES_THREAD_PROFILE=<level> */
} HermesThreadProfile;

extern const HermesThreadProfile HERMES_THREAD_PROFILES[];
extern const int HERMES_THREAD_PROFILE_COUNT;

/* Returns the profile for `import_name`, or NULL if it isn't a recognized
 * profile marker (i.e. it's a normal library import). */
const HermesThreadProfile *hermes_threadprofile_lookup(const char *import_name);

#endif
