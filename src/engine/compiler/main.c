#define _XOPEN_SOURCE 700 /* realpath */
#include "parser.h"
#include "codegen.h"
#include "arch.h"
#include "mempool.h"
#include "module.h"
#include "corelib.h"
#include "repo.h"
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
        "Uso: %s <entrada.hepy> [-o <salida.c>] [--compile <binario>] [--cc <compilador>]\n"
        "            [-I <dir>]... [--src <fichero.c|.S>]... [--repo-dir <dir>] [--core-dir <dir>]\n"
        "  -o <salida.c>        Ruta del .c generado (por defecto: <entrada>.c)\n"
        "  --compile <binario>  Tras transpilar, compila el .c al binario indicado\n"
        "  --cc <compilador>    Compilador a usar (por defecto: gcc). Se ignora si el\n"
        "                       .hepy importa alguna arquitectura reconocida (ver abajo)\n"
        "  -I <dir>              Directorio de includes adicional (repetible)\n"
        "  --src <fichero>       Fichero .c/.S adicional a enlazar junto al generado\n"
        "                        (repetible). Los del framework NO hacen falta: hepyc\n"
        "                        enlaza solo lo que pide cada import (ver corelib.c)\n"
        "  --repo-dir <dir>      Raiz del repo HERMES (src/ y modules/). Por defecto: la\n"
        "                        carpeta padre de la del binario hepyc (build/ -> raiz)\n"
        "  --core-dir <dir>      Donde buscar hermes_ctx_*.S (por defecto <repo>/src/framework/core)\n"
        "\n"
        "Arquitecturas reconocidas por import (ver docs/raw/Subconjunto-HERMESPY.v0.1.es.md):\n"
        "  Sin ningun import de arquitectura  -> un unico binario nativo (--compile tal cual)\n"
        "  Un import de arquitectura          -> un unico binario para esa arquitectura\n"
        "  Varios imports de arquitectura      -> un binario por cada una, con el nombre de\n"
        "                                        --compile sufijado por arquitectura\n"
        "\n"
        "Perfiles de memoria reconocidos por import ('light'/'medium'/'heavy'): fijan\n"
        "-DHERMES_POOL_BYTES=<N> en la compilacion (ver src/engine/compiler/mempool.c).\n"
        "Como maximo uno de los tres puede importarse. Ortogonal a la arquitectura.\n"
        "\n"
        "Modulos: 'import <nombre>' carga modules/<nombre>/module.hmod si existe (devices,\n"
        "fuentes, drivers y arquitecturas soportadas; ver docs/raw/hermes_modules.en.md).\n"
        "Si no existe, el import es un '#include \"<nombre>.h\"' normal.\n",
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
        fprintf(stderr, "hepyc: demasiados ficheros fuente (maximo %d)\n", MAX_BUILD_SOURCES);
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
                            const HermesArchProfile *arch) {
    l->count = 0;
    for (int i = 0; i < num_extra_sources; i++) sources_add(l, extra_sources[i]);

    for (int i = 0; i < hermes_corelib_selected_count(); i++) {
        const HermesCoreLib *lib = hermes_corelib_selected_at(i);
        for (int j = 0; lib->sources[j]; j++) {
            char sub[512];
            snprintf(sub, sizeof(sub), "src/%s", lib->sources[j]);
            sources_add(l, hermes_repo_path(repo_dir, sub));
        }
    }
    if (hermes_corelib_needs_ctx() && arch->ctx_src) {
        sources_add(l, hermes_repo_path(ctx_dir, arch->ctx_src));
    }

    for (int k = 0; k < hermes_module_count(); k++) {
        const HermesModule *m = hermes_module_at(k);
        for (int i = 0; i < m->num_sources; i++) {
            if (!m->sources[i].arch || strcmp(m->sources[i].arch, arch->import_name) == 0) {
                sources_add(l, m->sources[i].path);
            }
        }
    }
}

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
    printf("hepyc: ejecutando: %s\n", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "hepyc: fallo la compilacion del C generado (destino %s)\n", bin_path);
        exit(1);
    }
    printf("hepyc: binario generado %s\n", bin_path);
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
        } else if (strcmp(argv[i], "--repo-dir") == 0 && i + 1 < argc) {
            repo_override = argv[++i];
        } else {
            fprintf(stderr, "Argumento no reconocido: %s\n", argv[i]);
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
        if (hermes_arch_lookup(mod) || hermes_mempool_lookup(mod)) continue;
        if (hermes_corelib_select(mod)) continue;
        const HermesModule *m = hermes_module_load(modules_dir, mod);
        if (m) printf("hepyc: modulo '%s' (%s)\n", m->name, m->dir);
    }

    /* Collect the distinct recognized architecture imports, in the order
     * they first appear, to decide how many targets to build. */
    const HermesArchProfile *targets[HERMES_ARCH_PROFILE_COUNT];
    int num_targets = 0;
    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (item->kind != NODE_IMPORT) continue;
        const HermesArchProfile *prof = hermes_arch_lookup(item->as.import_stmt.module_name);
        if (!prof) continue;
        int already = 0;
        for (int j = 0; j < num_targets; j++) if (targets[j] == prof) already = 1;
        if (!already) targets[num_targets++] = prof;
    }

    /* Every imported module (and what it requires) must support every
     * target. Without an architecture import the build is native. */
    for (int k = 0; k < hermes_module_count(); k++) {
        const HermesModule *m = hermes_module_at(k);
        int ok = 1;
        const char *bad_arch = "native";
        if (num_targets == 0) {
            ok = hermes_module_supports_arch(m, "native");
        } else {
            for (int t = 0; t < num_targets && ok; t++) {
                if (!hermes_module_supports_arch(m, targets[t]->import_name)) {
                    ok = 0;
                    bad_arch = targets[t]->import_name;
                }
            }
        }
        if (!ok) {
            fprintf(stderr, "hepyc: el modulo '%s' no soporta la arquitectura '%s' (ver %s/module.hmod, clave 'archs')\n",
                    m->name, bad_arch, m->dir);
            return 1;
        }
    }

    char *c_code = codegen_generate(program);

    FILE *out = fopen(output_c_path, "w");
    if (!out) { perror(output_c_path); return 1; }
    fputs(c_code, out);
    fclose(out);
    printf("hepyc: generado %s\n", output_c_path);

    if (!compile_bin_path) {
        return 0;
    }

    /* Memory-profile import ('light'/'medium'/'heavy'): orthogonal to
     * architecture, so resolved once and applied to every target below
     * (including the no-architecture path). At most one allowed --
     * unlike architecture imports, this isn't a "build N variants"
     * selector, it's a single global numeric knob. */
    const HermesMemPoolProfile *mempool_prof = NULL;
    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (item->kind != NODE_IMPORT) continue;
        const HermesMemPoolProfile *mp = hermes_mempool_lookup(item->as.import_stmt.module_name);
        if (!mp) continue;
        if (mempool_prof && mempool_prof != mp) {
            fprintf(stderr,
                "hepyc: como maximo se puede importar un perfil de memoria ('%s' y '%s' son incompatibles)\n",
                mempool_prof->import_name, mp->import_name);
            return 1;
        }
        mempool_prof = mp;
    }
    char mempool_cflag[128] = "";
    if (mempool_prof) {
        snprintf(mempool_cflag, sizeof(mempool_cflag), "-DHERMES_POOL_BYTES=%ld", mempool_prof->pool_bytes);
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
        /* No architecture import: a single build with --cc (gcc by
         * default), exact --compile name. Treated as 'native' for what to
         * link -- the context-switch backend included, if needed. */
        collect_sources(&sources, extra_sources, num_extra_sources, repo_dir, core_dir,
                        hermes_arch_lookup("native"));
        run_build(cc_override, mempool_cflag, all_includes, num_all_includes, output_c_path,
                  &sources, compile_bin_path);
        return 0;
    }

    for (int t = 0; t < num_targets; t++) {
        const HermesArchProfile *prof = targets[t];

        char bin_path_buf[1024];
        const char *bin_path;
        if (num_targets == 1) {
            bin_path = compile_bin_path; /* single explicit target: exact name, no suffix */
        } else {
            snprintf(bin_path_buf, sizeof(bin_path_buf), "%s_%s%s",
                     compile_bin_path, prof->import_name, prof->bin_suffix);
            bin_path = bin_path_buf;
        }

        char combined_cflags[256];
        snprintf(combined_cflags, sizeof(combined_cflags), "%s %s", prof->extra_cflags, mempool_cflag);

        printf("hepyc: arquitectura '%s' -> %s\n", prof->import_name, bin_path);
        collect_sources(&sources, extra_sources, num_extra_sources, repo_dir, core_dir, prof);
        run_build(prof->cc, combined_cflags, all_includes, num_all_includes, output_c_path,
                  &sources, bin_path);
    }

    return 0;
}
