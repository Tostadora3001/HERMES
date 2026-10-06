#ifndef HERMES_ARCH_H
#define HERMES_ARCH_H

/* Fixed registry of "architecture marker" imports. A .hepy file importing
 * one of these (e.g. `import avr`) is not pulling in a real C header -- it
 * is telling hepyc which toolchain/target to compile the program for. See
 * docs/raw/Subconjunto-HERMESPY.v0.1.es.md and CLAUDE.md. */
typedef struct {
    const char *import_name;  /* recognized .hepy import, e.g. "avr" */
    const char *cc;            /* compiler to invoke for this target */
    const char *extra_cflags;  /* extra flags, may be "" */
    const char *ctx_src;       /* filename, within --core-dir, of this
                                   target's hermes_ctx_switch/_make backend;
                                   NULL if this target needs no extra source */
    const char *bin_suffix;    /* forced extension for the output binary
                                   when generating multiple targets, e.g.
                                   ".elf"; may be "" */
} HermesArchProfile;

extern const HermesArchProfile HERMES_ARCH_PROFILES[];
extern const int HERMES_ARCH_PROFILE_COUNT;

/* Returns the profile for `import_name`, or NULL if it isn't a recognized
 * architecture marker (i.e. it's a normal library import). */
const HermesArchProfile *hermes_arch_lookup(const char *import_name);

#endif
