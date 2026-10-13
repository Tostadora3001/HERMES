#include "module.h"
#include "arch.h"
#include "corelib.h"
#include "devicekind.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HERMES_MAX_MODULES 32

static HermesModule g_modules[HERMES_MAX_MODULES];
static int g_module_count = 0;

/* Names currently being loaded, to detect `requires` cycles. */
static const char *g_loading[HERMES_MAX_MODULES];
static int g_loading_count = 0;

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    memcpy(p, s, n);
    return p;
}

static char *path_join(const char *a, const char *b) {
    size_t n = strlen(a) + 1 + strlen(b) + 1;
    char *p = malloc(n);
    snprintf(p, n, "%s/%s", a, b);
    return p;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

const HermesModule *hermes_module_find(const char *name) {
    for (int i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i].name, name) == 0) return &g_modules[i];
    }
    return NULL;
}

int hermes_module_count(void) { return g_module_count; }

const HermesModule *hermes_module_at(int i) {
    return (i >= 0 && i < g_module_count) ? &g_modules[i] : NULL;
}

int hermes_module_supports_arch(const HermesModule *m, const char *arch) {
    for (int i = 0; i < m->num_archs; i++) {
        if (strcmp(m->archs[i], arch) == 0) return 1;
    }
    return 0;
}

/* --- .hmod parsing ------------------------------------------------------ */

typedef struct {
    const char *path;
    int line;
} HmodCtx;

static void hmod_fail(const HmodCtx *c, const char *msg, const char *detail) {
    if (c->line > 0) fprintf(stderr, "%s:%d: ", c->path, c->line);
    else fprintf(stderr, "%s: ", c->path); /* whole-file problem, e.g. a missing key */
    fprintf(stderr, "%s%s%s%s\n", msg,
            detail ? " '" : "", detail ? detail : "", detail ? "'" : "");
    exit(1);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* Splits `value` on whitespace into `out` (strings are copied). */
static int split_words(const HmodCtx *c, char *value, const char **out, int max) {
    int n = 0;
    for (char *tok = strtok(value, " \t"); tok; tok = strtok(NULL, " \t")) {
        if (n >= max) hmod_fail(c, "too many items in the list (at most 16)", NULL);
        out[n++] = xstrdup(tok);
    }
    return n;
}

static void check_arch_name(const HmodCtx *c, const char *arch) {
    if (!hermes_arch_lookup(arch)) hmod_fail(c, "unknown architecture", arch);
}

static void add_sources(const HmodCtx *c, HermesModule *m, const char *arch, char *value) {
    const char *files[HERMES_MODULE_MAX_LIST];
    int n = split_words(c, value, files, HERMES_MODULE_MAX_LIST);
    for (int i = 0; i < n; i++) {
        if (m->num_sources >= HERMES_MODULE_MAX_LIST) hmod_fail(c, "too many source files (at most 16)", NULL);
        char *full = path_join(m->dir, files[i]);
        if (!file_exists(full)) hmod_fail(c, "source file not found:", full);
        m->sources[m->num_sources].arch = arch ? xstrdup(arch) : NULL;
        m->sources[m->num_sources].path = full;
        m->num_sources++;
    }
}

/* `device = <kind> ctor=<constructor> type=<C handle type>` */
static void add_device(const HmodCtx *c, HermesModule *m, char *value) {
    const char *words[HERMES_MODULE_MAX_LIST];
    int n = split_words(c, value, words, HERMES_MODULE_MAX_LIST);
    if (n != 3) hmod_fail(c, "expected format: device = <kind> ctor=<constructor> type=<C type>", NULL);
    const char *kind = words[0];
    const char *ctor = NULL, *type = NULL;
    for (int i = 1; i < n; i++) {
        if (strncmp(words[i], "ctor=", 5) == 0) ctor = words[i] + 5;
        else if (strncmp(words[i], "type=", 5) == 0) type = words[i] + 5;
        else hmod_fail(c, "unknown device attribute", words[i]);
    }
    if (!ctor || !*ctor || !type || !*type) {
        hmod_fail(c, "expected format: device = <kind> ctor=<constructor> type=<C type>", NULL);
    }
    /* the kind names C symbols (device_<kind>_<method>), so it must be a
     * C identifier */
    for (const char *p = kind; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_') || (p == kind && isdigit((unsigned char)*p))) {
            hmod_fail(c, "the device kind must be a C identifier:", kind);
        }
    }
    if (!hermes_devicekind_register(ctor, kind, type)) hmod_fail(c, "could not register the device", ctor);
    m->num_devices++;
    int seen = 0;
    for (int i = 0; i < m->num_kinds; i++) if (strcmp(m->kinds[i], kind) == 0) seen = 1;
    if (!seen && m->num_kinds < HERMES_MODULE_MAX_LIST) m->kinds[m->num_kinds++] = kind;
}

/* `drivers = gpio timer ...`: core drivers (src/framework/drivers) this
 * module is built on; hepyc links them (see corelib.c, `driver:<name>`). */
static void add_drivers(const HmodCtx *c, HermesModule *m, char *value) {
    const char *names[HERMES_MODULE_MAX_LIST];
    int n = split_words(c, value, names, HERMES_MODULE_MAX_LIST);
    for (int i = 0; i < n; i++) {
        char key[128];
        snprintf(key, sizeof(key), "driver:%s", names[i]);
        if (!hermes_corelib_lookup(key)) hmod_fail(c, "unknown driver (see src/framework/drivers)", names[i]);
        if (m->num_drivers >= HERMES_MODULE_MAX_LIST) hmod_fail(c, "too many drivers (at most 16)", NULL);
        m->drivers[m->num_drivers++] = names[i];
    }
}

/* Every device must be able to deactivate itself: the module header must
 * define `device_<kind>_abort` for each kind it declares. A textual check
 * (hepyc doesn't parse C) -- the C side is enforced again by
 * HERMES_DEVICE_CLASS, which doesn't compile without it. */
static void check_abort_methods(const HmodCtx *c, const HermesModule *m, const char *header_path) {
    FILE *f = fopen(header_path, "rb");
    if (!f) { perror(header_path); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    size_t got = fread(text, 1, (size_t)size, f);
    text[got] = '\0';
    fclose(f);
    for (int i = 0; i < m->num_kinds; i++) {
        char sym[160];
        snprintf(sym, sizeof(sym), "device_%s_abort(", m->kinds[i]);
        if (!strstr(text, sym)) {
            hmod_fail(c, "the header doesn't define the mandatory method (every device must be "
                         "able to be deactivated; see docs/raw/hermes_modules.en.md)", sym);
        }
    }
    free(text);
}

static void parse_hmod(HermesModule *m, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }

    HmodCtx c = { path, 0 };
    char raw[1024];
    int has_name = 0;
    while (fgets(raw, sizeof(raw), f)) {
        c.line++;
        char *hash = strchr(raw, '#');
        if (hash) *hash = '\0';
        char *line = trim(raw);
        if (!*line) continue;

        char *eq = strchr(line, '=');
        if (!eq) hmod_fail(&c, "expected 'key = value'", NULL);
        *eq = '\0';
        char *key = trim(line);
        char *value = trim(eq + 1);

        if (strcmp(key, "name") == 0) {
            if (strcmp(value, m->name) != 0) {
                hmod_fail(&c, "'name' must match the module's folder name", value);
            }
            has_name = 1;
        } else if (strcmp(key, "description") == 0) {
            m->description = xstrdup(value);
        } else if (strcmp(key, "header") == 0) {
            m->header = xstrdup(value);
        } else if (strcmp(key, "archs") == 0) {
            m->num_archs = split_words(&c, value, m->archs, HERMES_MODULE_MAX_LIST);
            for (int i = 0; i < m->num_archs; i++) check_arch_name(&c, m->archs[i]);
        } else if (strcmp(key, "drivers") == 0) {
            add_drivers(&c, m, value);
        } else if (strcmp(key, "requires") == 0) {
            m->num_requires = split_words(&c, value, m->requires, HERMES_MODULE_MAX_LIST);
        } else if (strcmp(key, "device") == 0) {
            add_device(&c, m, value);
        } else if (strcmp(key, "sources") == 0) {
            add_sources(&c, m, NULL, value);
        } else if (strncmp(key, "sources.", 8) == 0) {
            check_arch_name(&c, key + 8);
            add_sources(&c, m, key + 8, value);
        } else {
            hmod_fail(&c, "unknown key", key);
        }
    }
    fclose(f);

    c.line = 0;
    if (!has_name) hmod_fail(&c, "missing mandatory key 'name'", NULL);
    if (m->num_archs == 0) hmod_fail(&c, "missing mandatory key 'archs'", NULL);
    char *header_path = path_join(m->dir, m->header);
    if (!file_exists(header_path)) hmod_fail(&c, "module header not found:", header_path);
    check_abort_methods(&c, m, header_path);
    free(header_path);
}

const HermesModule *hermes_module_load(const char *modules_dir, const char *name) {
    const HermesModule *existing = hermes_module_find(name);
    if (existing) return existing;

    char *dir = path_join(modules_dir, name);
    char *hmod_path = path_join(dir, "module.hmod");
    if (!file_exists(hmod_path)) {
        free(dir);
        free(hmod_path);
        return NULL;
    }

    for (int i = 0; i < g_loading_count; i++) {
        if (strcmp(g_loading[i], name) == 0) {
            fprintf(stderr, "%s: circular dependency between modules (through 'requires')\n", hmod_path);
            exit(1);
        }
    }
    if (g_module_count >= HERMES_MAX_MODULES || g_loading_count >= HERMES_MAX_MODULES) {
        fprintf(stderr, "hepyc: too many imported modules (at most %d)\n", HERMES_MAX_MODULES);
        exit(1);
    }

    HermesModule m;
    memset(&m, 0, sizeof(m));
    m.name = xstrdup(name);
    m.dir = dir;
    size_t hlen = strlen(name) + 3;
    char *default_header = malloc(hlen);
    snprintf(default_header, hlen, "%s.h", name);
    m.header = default_header;

    parse_hmod(&m, hmod_path);

    /* Dependencies first, so the final list is in a valid link/include
     * order (a module always appears after what it requires). */
    g_loading[g_loading_count++] = m.name;
    for (int i = 0; i < m.num_requires; i++) {
        if (!hermes_module_load(modules_dir, m.requires[i])) {
            fprintf(stderr, "%s: required module '%s' not found in %s\n",
                    hmod_path, m.requires[i], modules_dir);
            exit(1);
        }
    }
    g_loading_count--;
    free(hmod_path);

    /* Every module defines devices built on the runtime (allocation +
     * device registry), plus whatever drivers it declared. */
    hermes_corelib_select("@runtime");
    for (int i = 0; i < m.num_drivers; i++) {
        char key[128];
        snprintf(key, sizeof(key), "driver:%s", m.drivers[i]);
        hermes_corelib_select(key);
    }

    g_modules[g_module_count] = m;
    return &g_modules[g_module_count++];
}
