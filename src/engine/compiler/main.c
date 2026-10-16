#define _XOPEN_SOURCE 700 /* realpath */
#include "parser.h"
#include "codegen.h"
#include "arch.h"
#include "board.h"
#include "threadprofile.h"
#include "module.h"
#include "corelib.h"
#include "repo.h"
#include "diagnostics.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    size_t read_bytes = fread(buf, 1, size, f);
    buf[read_bytes] = '\0';
    fclose(f);
    return buf;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <input.hepy> [-o <output.c>] [--compile <binary>] [--cc <compiler>]\n"
        "            [-I <dir>]... [--src <file.c|.S>]... [--repo-dir <dir>] [--core-dir <dir>]\n"
        "  -o <output.c>        Path of the generated .c (default: <input>.c)\n"
        "  --compile <binary>   After transpiling, compile the .c into that binary\n"
        "  --cc <compiler>      Compiler to use (default: gcc). Ignored if the .hepy\n"
        "                       imports a known architecture (see below)\n"
        "  -I <dir>             Extra include directory (repeatable)\n"
        "  --src <file>         Extra .c/.S file to link with the generated one\n"
        "                       (repeatable). Framework files are NOT needed: hepyc\n"
        "                       links whatever each import needs (see corelib.c)\n"
        "  --repo-dir <dir>     Root of the HERMES repo (src/ and modules/). Default: the\n"
        "                       parent of the folder holding hepyc (build/ -> root)\n"
        "  --core-dir <dir>     Where to find hermes_ctx_*.S (default <repo>/src/framework/core)\n"
        "  --raw-diagnostics    Show the C compiler's messages as they are (by default\n"
        "                       hepyc rewrites the ones it knows in HERMESPY terms)\n"
        "\n"
        "Targets, by import (see docs/raw/hermes_boards.en.md):\n"
        "  'native' = the host; a board ('uno', 'mega'... = boards/<name>/) = that\n"
        "  board; 'avr' = the Uno.\n"
        "  No target import                -> one native binary (--compile as given)\n"
        "  One target import               -> one binary for that target\n"
        "  Several target imports          -> one binary per target, --compile\n"
        "                                     suffixed with the import name\n"
        "\n"
        "Thread-size profiles, by import ('light'/'medium'/'heavy'): the default size\n"
        "of each thread's block (heap + stack; AVR: 32+256 / 64+512 / 128+1024 B,\n"
        "host: 256+2K / 512+8K / 1K+16K; without one, medium). See hermes_thread.h.\n"
        "At most one. Independent of the architecture.\n"
        "\n"
        "Modules: 'import <name>' loads modules/<name>/module.hmod if it exists (devices,\n"
        "sources, drivers and supported architectures; see docs/raw/hermes_modules.en.md).\n"
        "Otherwise the import is a plain '#include \"<name>.h\"'.\n",
        prog);
}

/* --- source list for one build ------------------------------------------
 * Deduplicated by canonical path, so a caller still passing a framework
 * file with --src (the pre-corelib way) doesn't link it twice. */

#define MAX_BUILD_SOURCES 256

typedef struct {
    const char *paths[MAX_BUILD_SOURCES];
    char *canon[MAX_BUILD_SOURCES];
    int count;
} SourceList;

static void sources_add(SourceList *l, const char *path) {
    char buf[PATH_MAX];
    const char *canon = realpath(path, buf) ? buf : path;
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->canon[i], canon) == 0) return;
    }
    if (l->count >= MAX_BUILD_SOURCES) {
        fprintf(stderr, "hepyc: too many source files (at most %d)\n", MAX_BUILD_SOURCES);
        exit(1);
    }
    l->paths[l->count] = path;
    l->canon[l->count] = strdup(canon);
    l->count++;
}

/* Everything one target links besides the generated .c: the caller's
 * --src files, the selected core libraries (corelib.h), the context-switch
 * backend if any of them needs it, and every loaded module's sources that
 * apply to `arch` (common ones + `sources.<arch>`). */
static void collect_sources(SourceList *l, char **extra_sources, int num_extra_sources,
                            const char *repo_dir, const char *ctx_dir,
                            const HermesArchProfile *arch, const HermesPlatform *platform) {
    l->count = 0;
    for (int i = 0; i < num_extra_sources; i++) sources_add(l, extra_sources[i]);

    for (int i = 0; i < hermes_corelib_selected_count(); i++) {
        const HermesCoreLib *lib = hermes_corelib_selected_at(i);
        for (int j = 0; lib->sources[j]; j++) {
            /* `{platform}`: the target's implementation (e.g. of a driver) */
            char *src = hermes_corelib_source_for_platform(lib->sources[j], platform->name);
            char sub[512];
            snprintf(sub, sizeof(sub), "src/%s", src);
            char *full = hermes_repo_path(repo_dir, sub);
            FILE *probe = fopen(full, "r");
            if (!probe) {
                fprintf(stderr, "hepyc: '%s' has no implementation for platform '%s' (missing %s)\n",
                        lib->name, platform->name, full);
                exit(1);
            }
            fclose(probe);
            sources_add(l, full);
            free(src);
        }
    }
    if (hermes_corelib_needs_ctx() && arch->ctx_src) {
        sources_add(l, hermes_repo_path(ctx_dir, arch->ctx_src));
    }

    /* what every program for this chip links (startup...: platform.hplat) */
    for (int i = 0; i < platform->num_sources; i++) sources_add(l, platform->sources[i]);

    for (int k = 0; k < hermes_module_count(); k++) {
        const HermesModule *m = hermes_module_at(k);
        for (int i = 0; i < m->num_sources; i++) {
            if (!m->sources[i].arch || strcmp(m->sources[i].arch, arch->import_name) == 0) {
                sources_add(l, m->sources[i].path);
            }
        }
    }
}

/* --- build targets --------------------------------------------------------
 * One per binary: an architecture and the board it's built for (for
 * native, the board whose pin map the host mock uses). */
#define MAX_TARGETS 16

typedef struct {
    const char *import_name;          /* names the binary when there are several */
    const HermesArchProfile *arch;
    const HermesBoard *board;
    const HermesPlatform *platform;   /* the board's; native for the host */
} Target;

static const char *g_drivers_dir;     /* <repo>/src/framework/drivers */

static const HermesBoard *load_default_board(const char *boards_dir, const HermesArchProfile *arch) {
    const HermesBoard *b = hermes_board_load(boards_dir, g_drivers_dir, arch->default_board);
    if (!b) {
        fprintf(stderr, "hepyc: default board '%s' of architecture '%s' not found in %s\n",
                arch->default_board, arch->import_name, boards_dir);
        exit(1);
    }
    return b;
}

/* --raw-diagnostics: show the C compiler's messages as they are */
static int g_raw_diagnostics = 0;

static void run_build(const char *cc, const char *extra_cflags,
                       const char **include_dirs, int num_includes,
                       const char *output_c_path,
                       const SourceList *sources,
                       const char *bin_path) {
    char cmd[16384];
    int offset = snprintf(cmd, sizeof(cmd), "%s -std=c11 -Wall -Wextra", cc);
    if (extra_cflags && extra_cflags[0]) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset, " %s", extra_cflags);
    }
    for (int i = 0; i < num_includes; i++) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset, " -I%s", include_dirs[i]);
    }
    offset += snprintf(cmd + offset, sizeof(cmd) - offset, " %s", output_c_path);
    for (int i = 0; i < sources->count; i++) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset, " %s", sources->paths[i]);
    }
    snprintf(cmd + offset, sizeof(cmd) - offset, " -o %s", bin_path);
    printf("hepyc: running: %s\n", cmd);
    int ret = hermes_run_compiler(cmd, g_raw_diagnostics);
    if (ret != 0) {
        fprintf(stderr, "hepyc: compiling the generated C failed (target %s)\n", bin_path);
        exit(1);
    }
    printf("hepyc: built %s\n", bin_path);
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const char *input_path = argv[1];
    char *output_c_path = NULL;
    char *compile_bin_path = NULL;
    const char *cc_override = "gcc";
    char *include_dirs[32];
    int num_includes = 0;
    char *extra_sources[32];
    int num_extra_sources = 0;
    const char *core_dir = NULL;
    const char *repo_override = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_c_path = argv[++i];
        } else if (strcmp(argv[i], "--compile") == 0 && i + 1 < argc) {
            compile_bin_path = argv[++i];
        } else if (strcmp(argv[i], "--cc") == 0 && i + 1 < argc) {
            cc_override = argv[++i];
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            if (num_includes < 32) include_dirs[num_includes++] = argv[++i];
        } else if (strcmp(argv[i], "--src") == 0 && i + 1 < argc) {
            if (num_extra_sources < 32) extra_sources[num_extra_sources++] = argv[++i];
        } else if (strcmp(argv[i], "--core-dir") == 0 && i + 1 < argc) {
            core_dir = argv[++i];
        } else if (strcmp(argv[i], "--raw-diagnostics") == 0) {
            g_raw_diagnostics = 1;
        } else if (strcmp(argv[i], "--repo-dir") == 0 && i + 1 < argc) {
            repo_override = argv[++i];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    char default_output[1024];
    if (!output_c_path) {
        snprintf(default_output, sizeof(default_output), "%s.c", input_path);
        output_c_path = default_output;
    }

    const char *repo_dir = hermes_repo_dir(argv[0], repo_override);
    const char *modules_dir = hermes_repo_path(repo_dir, "modules");
    const char *boards_dir = hermes_repo_path(repo_dir, "boards");
    g_drivers_dir = hermes_repo_path(repo_dir, "src/framework/drivers");
    if (!core_dir) core_dir = hermes_repo_path(repo_dir, "src/framework/core");

    char *src = read_file(input_path);
    Node *program = parse_program(src, input_path);

    /* Resolve every import before codegen: core libraries (what to link)
     * and modules (they also register the device kinds, devicekind.h, that
     * `device x = ctor(...)` resolves against; codegen asks module.h which
     * imports are modules). */
    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (item->kind != NODE_IMPORT) continue;
        const char *mod = item->as.import_stmt.module_name;
        if (hermes_arch_lookup(mod) || hermes_threadprofile_lookup(mod)) continue;
        if (hermes_board_load(boards_dir, g_drivers_dir, mod)) continue;
        if (hermes_corelib_select(mod)) continue;
        const HermesModule *m = hermes_module_load(modules_dir, mod);
        if (m) printf("hepyc: module '%s' (%s)\n", m->name, m->dir);
    }

    {
        const char *names[256];
        int n = 0;
        for (int i = 0; i < program->as.program.items.count && n < 256; i++) {
            Node *item = program->as.program.items.items[i];
            if (item->kind == NODE_IMPORT) names[n++] = item->as.import_stmt.module_name;
        }
        if (!hermes_corelib_check_rules(names, n)) return 1;
    }

    /* Collect the build targets, in the order their imports first appear:
     * `native` (the host), a board (`import uno`), or another architecture
     * (`import avr` = its default board). Two imports naming the same
     * build (e.g. `avr` and `uno`) give one target. */
    Target targets[MAX_TARGETS];
    int num_targets = 0;
    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (item->kind != NODE_IMPORT) continue;
        const char *mod = item->as.import_stmt.module_name;
        Target t = { mod, NULL, NULL, NULL };
        const HermesArchProfile *prof = hermes_arch_lookup(mod);
        if (prof) {
            t.arch = prof;
            if (prof->mcu_flag) t.board = load_default_board(boards_dir, prof);
        } else if ((t.board = hermes_board_find(mod)) != NULL) {
            t.arch = t.board->arch;
        } else {
            continue;
        }
        int already = 0;
        for (int j = 0; j < num_targets; j++) {
            if (targets[j].arch == t.arch && targets[j].board == t.board) already = 1;
        }
        if (!already && num_targets < MAX_TARGETS) targets[num_targets++] = t;
    }

    /* The host build's pin map (its GPIO mock): the first board imported,
     * else the native profile's default board. */
    const HermesArchProfile *native = hermes_arch_lookup("native");
    const HermesBoard *native_board = NULL;
    for (int j = 0; j < num_targets && !native_board; j++) native_board = targets[j].board;
    if (!native_board) native_board = load_default_board(boards_dir, native);
    const HermesPlatform *native_platform = hermes_platform_load(g_drivers_dir, "native", native);
    for (int j = 0; j < num_targets; j++) {
        if (!targets[j].board) targets[j].board = native_board;
        targets[j].platform = targets[j].arch->mcu_flag ? targets[j].board->platform : native_platform;
    }

    /* Every imported module (and what it requires) must support every
     * target's architecture. Without a target import the build is native. */
    for (int k = 0; k < hermes_module_count(); k++) {
        const HermesModule *m = hermes_module_at(k);
        int ok = 1;
        const char *bad_arch = "native";
        if (num_targets == 0) {
            ok = hermes_module_supports_arch(m, "native");
        } else {
            for (int t = 0; t < num_targets && ok; t++) {
                if (!hermes_module_supports_arch(m, targets[t].arch->import_name)) {
                    ok = 0;
                    bad_arch = targets[t].arch->import_name;
                }
            }
        }
        if (!ok) {
            fprintf(stderr, "hepyc: module '%s' doesn't support architecture '%s' (see %s/module.hmod, key 'archs')\n",
                    m->name, bad_arch, m->dir);
            return 1;
        }
    }

    char *c_code = codegen_generate(program, input_path);

    FILE *out = fopen(output_c_path, "w");
    if (!out) { perror(output_c_path); return 1; }
    fputs(c_code, out);
    fclose(out);
    printf("hepyc: generated %s\n", output_c_path);

    if (!compile_bin_path) {
        return 0;
    }

    /* Thread-size profile import ('light'/'medium'/'heavy'): orthogonal to
     * architecture, so resolved once and applied to every target below
     * (including the no-architecture path). At most one allowed. */
    const HermesThreadProfile *thread_prof = NULL;
    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (item->kind != NODE_IMPORT) continue;
        const HermesThreadProfile *mp = hermes_threadprofile_lookup(item->as.import_stmt.module_name);
        if (!mp) continue;
        if (thread_prof && thread_prof != mp) {
            fprintf(stderr,
                "hepyc: at most one thread-size profile can be imported ('%s' and '%s' are incompatible)\n",
                thread_prof->import_name, mp->import_name);
            return 1;
        }
        thread_prof = mp;
    }
    char profile_cflag[512] = "";
    if (thread_prof) {
        snprintf(profile_cflag, sizeof(profile_cflag), "-DHERMES_THREAD_PROFILE=%d", thread_prof->level);
    }
    /* with the UART driver linked, the framework's error messages go out
     * through it on AVR (src/framework/core/hermes_diag.h) */
    if (hermes_corelib_is_selected("driver:uart")) {
        size_t used = strlen(profile_cflag);
        snprintf(profile_cflag + used, sizeof(profile_cflag) - used, "%s-DHERMES_DIAG_UART",
                 used ? " " : "");
    }
    /* timer roles: the timer driver can then run the system timer on any
     * timer (src/framework/drivers/hermes_drv_timers.h) */
    if (hermes_corelib_is_selected("hermes_timers")) {
        size_t used = strlen(profile_cflag);
        snprintf(profile_cflag + used, sizeof(profile_cflag) - used, "%s-DHERMES_TIMERS",
                 used ? " " : "");
    }

    /* the I2C slave's interrupt and buffers (64 B of RAM on AVR) only if
     * the program creates one: an ISR is always linked once it exists */
    if (hermes_corelib_is_selected("hermes_i2c") && codegen_calls("i2c_slave")) {
        size_t used = strlen(profile_cflag);
        snprintf(profile_cflag + used, sizeof(profile_cflag) - used, "%s-DHERMES_I2C_SLAVE",
                 used ? " " : "");
    }

    /* hermes_net's optional parts, only if the program uses them (each
     * interface leaves them out otherwise: an AVR's flash): a TCP server,
     * DNS (resolve/dns, or a host named in tcp()/send_to()), DHCP */
    if (hermes_corelib_is_selected("hermes_net")) {
        const char *flags[3] = {
            codegen_calls_method("listen") ? "-DHERMES_NET_SERVER" : NULL,
            (codegen_calls_method("resolve") || codegen_calls_method("dns") || codegen_names_hosts()) ? "-DHERMES_NET_DNS" : NULL,
            codegen_calls_method("dhcp") ? "-DHERMES_NET_DHCP" : NULL,
        };
        for (int i = 0; i < 3; i++) {
            if (!flags[i]) continue;
            size_t used = strlen(profile_cflag);
            snprintf(profile_cflag + used, sizeof(profile_cflag) - used, "%s%s", used ? " " : "", flags[i]);
        }
    }

    /* device handles checked by serial number (hermes_device.h, "handles"):
     * only if the program asks for it with device_handles_on(); otherwise
     * a handle is the device's address (decided with the user) */
    if (codegen_calls("device_handles_on")) {
        size_t used = strlen(profile_cflag);
        snprintf(profile_cflag + used, sizeof(profile_cflag) - used, "%s-DHERMES_HANDLES",
                 used ? " " : "");
    }

    /* os_priority: the thread control block carries the use record of
     * the fair share among equal priorities (hermes_thread.h) */
    if (hermes_corelib_is_selected("os_priority")) {
        size_t used = strlen(profile_cflag);
        snprintf(profile_cflag + used, sizeof(profile_cflag) - used, "%s-DHERMES_OS_PRIORITY",
                 used ? " " : "");
    }

    /* os_deadline: the thread control block carries the periodic fields
     * (src/framework/core/hermes_thread.h), and mutexes order by deadline */
    if (hermes_corelib_is_selected("os_deadline")) {
        size_t used = strlen(profile_cflag);
        snprintf(profile_cflag + used, sizeof(profile_cflag) - used, "%s-DHERMES_OS_DEADLINE",
                 used ? " " : "");
    }

    /* device details: every kind's describe fills the dump's fields only
     * when the program can dump (src/framework/devices/hermes_device.h) */
    if (hermes_corelib_is_selected("hermes_devices")) {
        size_t used = strlen(profile_cflag);
        snprintf(profile_cflag + used, sizeof(profile_cflag) - used, "%s-DHERMES_DEVICES",
                 used ? " " : "");
    }

    /* Include dirs for every build: the caller's, then the framework's own
     * (so no -I is needed for anything that ships with HERMES), then the
     * modules dir (generated code includes "<module>/<header>"). */
    static const char *const FRAMEWORK_INCLUDE_SUBDIRS[] = {
        "src/useful", "src/framework/core", "src/framework/devices", "src/framework/drivers",
    };
    const char *all_includes[48];
    int num_all_includes = 0;
    for (int i = 0; i < num_includes; i++) all_includes[num_all_includes++] = include_dirs[i];
    for (size_t i = 0; i < sizeof(FRAMEWORK_INCLUDE_SUBDIRS) / sizeof(FRAMEWORK_INCLUDE_SUBDIRS[0]); i++) {
        all_includes[num_all_includes++] = hermes_repo_path(repo_dir, FRAMEWORK_INCLUDE_SUBDIRS[i]);
    }
    if (hermes_module_count() > 0) all_includes[num_all_includes++] = modules_dir;

    SourceList sources;

    if (num_targets == 0) {
        /* No target import: a single build with --cc (gcc by default),
         * exact --compile name. Treated as 'native' for what to link -- the
         * context-switch backend and driver implementations included. */
        collect_sources(&sources, extra_sources, num_extra_sources, repo_dir, core_dir, native,
                        native_platform);
        all_includes[num_all_includes] = native_board->dir;
        /* the runtime's cores are pthreads on the host */
        char legacy_cflags[544]; /* "-pthread " + profile_cflag */
        snprintf(legacy_cflags, sizeof(legacy_cflags), "%s%s",
                 hermes_corelib_needs_ctx() ? "-pthread " : "", profile_cflag);
        run_build(cc_override, legacy_cflags, all_includes, num_all_includes + 1, output_c_path,
                  &sources, compile_bin_path);
        return 0;
    }

    for (int t = 0; t < num_targets; t++) {
        const HermesArchProfile *prof = targets[t].arch;
        const HermesBoard *board = targets[t].board;

        char bin_path_buf[1024];
        const char *bin_path;
        if (num_targets == 1) {
            bin_path = compile_bin_path; /* single explicit target: exact name, no suffix */
        } else {
            snprintf(bin_path_buf, sizeof(bin_path_buf), "%s_%s%s",
                     compile_bin_path, targets[t].import_name, prof->bin_suffix);
            bin_path = bin_path_buf;
        }

        /* architecture flags, then the board's chip and clock */
        char combined_cflags[1024];
        int n = snprintf(combined_cflags, sizeof(combined_cflags), "%s", prof->extra_cflags);
        if (prof->mcu_flag) {
            n += snprintf(combined_cflags + n, sizeof(combined_cflags) - n, " ");
            n += snprintf(combined_cflags + n, sizeof(combined_cflags) - n, prof->mcu_flag, board->mcu);
            n += snprintf(combined_cflags + n, sizeof(combined_cflags) - n, " -DF_CPU=%sUL", board->f_cpu);
        }
        if (targets[t].platform->cflags[0]) {
            n += snprintf(combined_cflags + n, sizeof(combined_cflags) - n, " %s", targets[t].platform->cflags);
        }
        snprintf(combined_cflags + n, sizeof(combined_cflags) - n, " %s", profile_cflag);

        if (prof->mcu_flag) {
            printf("hepyc: target '%s' (board %s, %s, platform %s) -> %s\n", targets[t].import_name,
                   board->name, board->mcu, targets[t].platform->name, bin_path);
        } else {
            printf("hepyc: target '%s' (pins of board %s) -> %s\n", targets[t].import_name, board->name, bin_path);
        }
        collect_sources(&sources, extra_sources, num_extra_sources, repo_dir, core_dir, prof,
                        targets[t].platform);
        all_includes[num_all_includes] = board->dir; /* its hermes_board.h */
        run_build(prof->cc, combined_cflags, all_includes, num_all_includes + 1, output_c_path,
                  &sources, bin_path);
    }

    return 0;
}
