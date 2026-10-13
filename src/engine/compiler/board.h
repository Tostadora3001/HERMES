#ifndef HERMES_BOARD_LOADER_H
#define HERMES_BOARD_LOADER_H

#include "arch.h"

/* HERMES boards: one folder per board in the repo's top-level `boards/`:
 *
 *   boards/<name>/board.hboard   <- description, read by hepyc (this file)
 *   boards/<name>/hermes_board.h <- pin map (HERMES_BOARD_PINS), included
 *                                   by the drivers
 *
 * `import <name>` in a .hepy, if boards/<name>/board.hboard exists, makes
 * that board a build target: its architecture's toolchain, `-mmcu=<mcu>`
 * (the architecture's mcu_flag) and `-DF_CPU=<f_cpu>UL`, and
 * -I boards/<name> so the drivers find its hermes_board.h. The host build
 * (native) uses the pin map of the first board imported (without one, the
 * native profile's default_board), so its GPIO mock has the same pins.
 *
 * .hboard format: one `key = value` per line, `#` starts a comment, unknown
 * keys are an error. Keys: name (= folder name), description, arch (an
 * architecture with boards: arch.c, mcu_flag != NULL), platform (the
 * driver folder; default: the arch's name), mcu, f_cpu (Hz, integer). All
 * but description and platform are mandatory. See
 * docs/raw/hermes_boards.en.md.
 *
 * Platforms: the chip family whose registers the drivers know, i.e. the
 * folder src/framework/drivers/<platform>/ (avr, esp32c3, native). It may
 * hold a platform.hplat (same format; keys: name, arch, sources, cflags)
 * with what every program for that chip links whether it uses drivers or
 * not -- startup code, linker script:
 *   sources = files in the platform folder, linked into every build
 *   cflags  = extra compiler/linker flags; `{dir}` = the platform folder
 * A platform without that file (avr, native) adds nothing. */

#define HERMES_PLATFORM_MAX_SOURCES 8

typedef struct {
    const char *name;
    const char *dir;                 /* full path of src/framework/drivers/<name> */
    const char *sources[HERMES_PLATFORM_MAX_SOURCES]; /* full paths */
    int num_sources;
    const char *cflags;              /* "" if none */
} HermesPlatform;

/* Loads the platform `name` from `drivers_dir` (its platform.hplat if
 * there is one). The folder must exist. `arch` (may be NULL) must match
 * the .hplat's `arch`. Errors are fatal. Loaded once per name. */
const HermesPlatform *hermes_platform_load(const char *drivers_dir, const char *name,
                                           const HermesArchProfile *arch);

typedef struct {
    const char *name;
    const char *description;
    const char *dir;                 /* full path of boards/<name> */
    const HermesArchProfile *arch;
    const HermesPlatform *platform;
    const char *mcu;
    const char *f_cpu;               /* digits only */
} HermesBoard;

/* Loads boards/<name>. Returns NULL if `boards_dir/<name>/board.hboard`
 * doesn't exist (= not a board). Any error inside an existing board.hboard
 * is fatal (diagnostic + exit(1)). Loading the same board twice returns
 * the already-loaded one. */
const HermesBoard *hermes_board_load(const char *boards_dir, const char *drivers_dir,
                                     const char *name);

/* Already-loaded board by name, or NULL. */
const HermesBoard *hermes_board_find(const char *name);

#endif
