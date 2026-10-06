#include "parser.h"
#include "codegen.h"
#include "arch.h"
#include "mempool.h"
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
        "            [-I <dir>]... [--src <fichero.c|.S>]... [--core-dir <dir>]\n"
        "  -o <salida.c>        Ruta del .c generado (por defecto: <entrada>.c)\n"
        "  --compile <binario>  Tras transpilar, compila el .c al binario indicado\n"
        "  --cc <compilador>    Compilador a usar (por defecto: gcc). Se ignora si el\n"
        "                       .hepy importa alguna arquitectura reconocida (ver abajo)\n"
        "  -I <dir>              Directorio de includes adicional (repetible)\n"
        "  --src <fichero>       Fichero .c/.S adicional a enlazar junto al generado\n"
        "                        (repetible; para bibliotecas del framework que no son\n"
        "                        solo-cabecera, p. ej. hermes_thread.c/hermes_sem.c)\n"
        "  --core-dir <dir>      Directorio donde buscar los backends de context-switch\n"
        "                        especificos de arquitectura (hermes_ctx_*.S). Requerido\n"
        "                        si el .hepy importa 'native' y/o 'avr'\n"
        "\n"
        "Arquitecturas reconocidas por import (ver docs/raw/Subconjunto-HERMESPY.v0.1.es.md):\n"
        "  Sin ningun import de arquitectura  -> un unico binario nativo (--compile tal cual)\n"
        "  Un import de arquitectura          -> un unico binario para esa arquitectura\n"
        "  Varios imports de arquitectura      -> un binario por cada una, con el nombre de\n"
        "                                        --compile sufijado por arquitectura\n"
        "\n"
        "Perfiles de memoria reconocidos por import ('light'/'medium'/'heavy'): fijan\n"
        "-DHERMES_POOL_BYTES=<N> en la compilacion (ver src/engine/compiler/mempool.c).\n"
        "Como maximo uno de los tres puede importarse. Ortogonal a la arquitectura.\n",
        prog);
}

static void run_build(const char *cc, const char *extra_cflags,
                       char **include_dirs, int num_includes,
                       const char *output_c_path,
                       char **extra_sources, int num_extra_sources,
                       const char *arch_ctx_src, /* NULL if none */
                       const char *bin_path) {
    char cmd[4096];
    int offset = snprintf(cmd, sizeof(cmd), "%s -std=c11 -Wall -Wextra", cc);
    if (extra_cflags && extra_cflags[0]) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset, " %s", extra_cflags);
    }
    for (int i = 0; i < num_includes; i++) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset, " -I%s", include_dirs[i]);
    }
    offset += snprintf(cmd + offset, sizeof(cmd) - offset, " %s", output_c_path);
    for (int i = 0; i < num_extra_sources; i++) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset, " %s", extra_sources[i]);
    }
    if (arch_ctx_src) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset, " %s", arch_ctx_src);
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
    char *core_dir = NULL;

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

    char *src = read_file(input_path);
    Node *program = parse_program(src, input_path);
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
     * (including the legacy single-build path). At most one allowed --
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
    char mempool_cflag[64] = "";
    if (mempool_prof) {
        snprintf(mempool_cflag, sizeof(mempool_cflag), "-DHERMES_POOL_BYTES=%ld", mempool_prof->pool_bytes);
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

    if (num_targets == 0) {
        /* Legacy path: no architecture import present. Single native-style
         * build, exactly as before this feature existed -- no automatic
         * source injection, caller controls --src/--cc/-I directly. The
         * memory-profile flag (if any) still applies. */
        run_build(cc_override, mempool_cflag, include_dirs, num_includes, output_c_path,
                  extra_sources, num_extra_sources, NULL, compile_bin_path);
        return 0;
    }

    for (int t = 0; t < num_targets; t++) {
        const HermesArchProfile *prof = targets[t];
        char *arch_ctx_src = NULL;
        char arch_ctx_src_buf[1024];
        if (prof->ctx_src) {
            if (!core_dir) {
                fprintf(stderr,
                    "hepyc: se requiere --core-dir para resolver %s (arquitectura '%s')\n",
                    prof->ctx_src, prof->import_name);
                return 1;
            }
            snprintf(arch_ctx_src_buf, sizeof(arch_ctx_src_buf), "%s/%s", core_dir, prof->ctx_src);
            arch_ctx_src = arch_ctx_src_buf;
        }

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
        run_build(prof->cc, combined_cflags, include_dirs, num_includes, output_c_path,
                  extra_sources, num_extra_sources, arch_ctx_src, bin_path);
    }

    return 0;
}
