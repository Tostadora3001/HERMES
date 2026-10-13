#ifndef HERMES_ARCH_H
#define HERMES_ARCH_H

/* Fixed registry of the architectures (toolchain families). An
 * architecture is chosen through a board (board.h: `import uno` -> avr) or
 * directly: `import native` (the host) and `import avr` (= its default
 * board, the Uno). Imports, not real C headers: they tell hepyc which
 * toolchain/target to compile for. See docs/raw/hermes_boards.en.md and
 * docs/raw/Subconjunto-HERMESPY.v0.1.es.md.
 *
 * The architecture's name is also the default platform (the folder of the
 * driver implementations, src/framework/drivers/<platform>/: corelib.h,
 * `{platform}`) of its boards; a board may name another (board.h). */
typedef struct {
    const char *import_name;  /* recognized .hepy import, e.g. "avr" */
    const char *cc;            /* compiler to invoke for this target */
    const char *extra_cflags;  /* extra flags, may be "" */
    const char *mcu_flag;      /* printf format of the chip flag, filled
                                   with the board's mcu (e.g. "-mmcu=%s");
                                   NULL: the architecture has no boards */
    const char *ctx_src;       /* filename, within --core-dir, of this
                                   target's hermes_ctx_switch/_make backend;
                                   NULL if this target needs no extra source */
    const char *bin_suffix;    /* forced extension for the output binary
                                   when generating multiple targets, e.g.
                                   ".elf"; may be "" */
    const char *default_board; /* `import <arch>` alone builds for this
                                   board; for native, the board whose pin
                                   map the host mock uses when no board is
                                   imported */
} HermesArchProfile;

extern const HermesArchProfile HERMES_ARCH_PROFILES[];
extern const int HERMES_ARCH_PROFILE_COUNT;

/* Returns the profile for `import_name`, or NULL if it isn't a recognized
 * architecture marker (i.e. it's a normal library import). */
const HermesArchProfile *hermes_arch_lookup(const char *import_name);

#endif
