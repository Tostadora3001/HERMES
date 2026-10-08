#ifndef HERMES_MODULE_H
#define HERMES_MODULE_H

/* HERMES modules: self-contained hardware support packages living in the
 * repo's top-level `modules/` directory, one folder per module:
 *
 *   modules/<name>/module.hmod   <- description, read by hepyc (this file)
 *   modules/<name>/<name>.h      <- C API (device struct, constructor,
 *                                   device_<kind>_<method> functions)
 *   modules/<name>/<file>.c      <- optional implementation sources
 *
 * `import <name>` in a .hepy loads modules/<name>/module.hmod (if that
 * folder exists -- otherwise the import keeps its old meaning, a plain
 * `#include "<name>.h"`). Loading a module:
 *   - registers the device kinds it declares (devicekind.h), so
 *     `device x = ctor(...)` / `x.method(...)` work for them;
 *   - makes the generated .c `#include "<name>/<header>"` (hepyc adds
 *     -I<modules dir> to every build);
 *   - adds its sources (common + per-architecture) to every build;
 *   - loads its `requires` recursively;
 *   - makes hepyc link the runtime (alloc + device registry) and the core
 *     drivers listed in `drivers` (corelib.h);
 *   - lets hepyc reject a build for an architecture the module doesn't
 *     list in `archs`;
 *   - checks the module header defines `device_<kind>_abort` for every
 *     device kind it declares (mandatory abort, see hermes_device.h).
 *
 * .hmod format: one `key = value` per line, `#` starts a comment, unknown
 * keys are an error (catches typos). See docs/raw/hermes_modules.en.md for
 * the full reference. */

#define HERMES_MODULE_MAX_LIST 16

typedef struct {
    const char *arch; /* NULL = every architecture */
    const char *path; /* full path (modules dir + module folder + file) */
} HermesModuleSource;

typedef struct {
    const char *name;
    const char *description;
    const char *dir;    /* full path of modules/<name> */
    const char *header; /* file name inside dir; default "<name>.h" */

    const char *archs[HERMES_MODULE_MAX_LIST];
    int num_archs;

    const char *requires[HERMES_MODULE_MAX_LIST];
    int num_requires;

    const char *drivers[HERMES_MODULE_MAX_LIST]; /* core drivers it uses */
    int num_drivers;

    HermesModuleSource sources[HERMES_MODULE_MAX_LIST];
    int num_sources;

    int num_devices; /* how many `device = ...` lines it declared */
    const char *kinds[HERMES_MODULE_MAX_LIST]; /* distinct device kinds it declares */
    int num_kinds;
} HermesModule;

/* Loads modules/<name> (and, recursively, what it requires) from
 * `modules_dir`. Returns NULL if `modules_dir/<name>/module.hmod` doesn't
 * exist (= not a module). Any error inside an existing module.hmod is
 * fatal (diagnostic + exit(1)). Loading the same module twice returns the
 * already-loaded one. */
const HermesModule *hermes_module_load(const char *modules_dir, const char *name);

/* Already-loaded module by name, or NULL. */
const HermesModule *hermes_module_find(const char *name);

int hermes_module_count(void);
const HermesModule *hermes_module_at(int i);

/* 1 if the module lists `arch` (an architecture import name) in `archs`. */
int hermes_module_supports_arch(const HermesModule *m, const char *arch);

#endif
