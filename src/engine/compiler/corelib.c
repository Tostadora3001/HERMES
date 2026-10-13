#include "corelib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const HermesCoreLib HERMES_CORELIBS[] = {
    /* internal: everything a thread or a device needs at runtime -- the
     * thread library (cores, run/launch/wait), semaphores,
     * hermes_runtime_alloc (in hermes_thread.c) and the device registry. */
    { "@runtime",
      { "framework/core/hermes_thread.c", "framework/core/hermes_sem.c",
        "framework/devices/hermes_device.c", "framework/core/hermes_diag.c", NULL },
      { NULL }, 1 },

    /* drivers (src/framework/drivers): the common part + the target's
     * implementation, drivers/<platform>/hermes_drv_<x>_<platform>.c */
    { "driver:gpio",  { "framework/drivers/hermes_drv_gpio.c",
                        "framework/drivers/{platform}/hermes_drv_gpio_{platform}.c", NULL },  { NULL }, 0 },
    { "driver:timer", { "framework/drivers/hermes_drv_timer.c",
                        "framework/drivers/{platform}/hermes_drv_timer_{platform}.c", NULL }, { NULL }, 0 },
    { "driver:uart",  { "framework/drivers/hermes_drv_uart.c",
                        "framework/drivers/{platform}/hermes_drv_uart_{platform}.c", NULL },  { NULL }, 0 },

    /* importable libraries */
    { "hermes_threads", { NULL }, { "@runtime", NULL }, 0 },
    { "hermes_parallel", { "framework/core/hermes_parallel.c", NULL }, { "@runtime", NULL }, 0 },

    /* the mini OS and its schedulers: what they need from each other is
     * NOT pulled in automatically -- the user must write every import
     * (decided with the user, so the program states what it contains);
     * see hermes_corelib_check_rules */
    { "hermes_os",   { "framework/core/hermes_os.c", NULL },   { "@runtime", "driver:timer", NULL }, 0 },
    { "os_priority", { "framework/core/os_priority.c", NULL }, { NULL }, 0 },
    { "hermes_devices", { NULL }, { "@runtime", NULL }, 0 },
    { "hermes_time",    { NULL }, { "driver:timer", NULL }, 0 },
    { "hermes_uart",    { NULL }, { "driver:uart", "@runtime", NULL }, 0 },
};
#define HERMES_CORELIB_COUNT ((int)(sizeof(HERMES_CORELIBS) / sizeof(HERMES_CORELIBS[0])))

static const HermesCoreLib *g_selected[HERMES_CORELIB_COUNT];
static int g_selected_count = 0;

const HermesCoreLib *hermes_corelib_lookup(const char *name) {
    for (int i = 0; i < HERMES_CORELIB_COUNT; i++) {
        if (strcmp(HERMES_CORELIBS[i].name, name) == 0) return &HERMES_CORELIBS[i];
    }
    return NULL;
}

int hermes_corelib_select(const char *name) {
    const HermesCoreLib *lib = hermes_corelib_lookup(name);
    if (!lib) return 0;
    for (int i = 0; i < g_selected_count; i++) {
        if (g_selected[i] == lib) return 1;
    }
    for (int i = 0; lib->requires[i]; i++) hermes_corelib_select(lib->requires[i]);
    g_selected[g_selected_count++] = lib;
    return 1;
}

int hermes_corelib_selected_count(void) { return g_selected_count; }

int hermes_corelib_is_selected(const char *name) {
    for (int i = 0; i < g_selected_count; i++) {
        if (strcmp(g_selected[i]->name, name) == 0) return 1;
    }
    return 0;
}

const HermesCoreLib *hermes_corelib_selected_at(int i) {
    return (i >= 0 && i < g_selected_count) ? g_selected[i] : NULL;
}

/* Imports that must be written explicitly together (never added by hepyc). */
static const struct { const char *import; const char *needs; } HERMES_IMPORT_RULES[] = {
    { "hermes_os",   "hermes_threads" },
    { "hermes_os",   "@scheduler" },     /* one of HERMES_SCHEDULERS */
    { "os_priority", "hermes_os" },
};
static const char *const HERMES_SCHEDULERS[] = { "os_priority", NULL };

static int has_import(const char *const *imports, int n, const char *name) {
    for (int i = 0; i < n; i++) if (strcmp(imports[i], name) == 0) return 1;
    return 0;
}

int hermes_corelib_check_rules(const char *const *imports, int n) {
    int ok = 1;
    int schedulers = 0;
    for (int s = 0; HERMES_SCHEDULERS[s]; s++) schedulers += has_import(imports, n, HERMES_SCHEDULERS[s]);
    for (size_t r = 0; r < sizeof(HERMES_IMPORT_RULES) / sizeof(HERMES_IMPORT_RULES[0]); r++) {
        if (!has_import(imports, n, HERMES_IMPORT_RULES[r].import)) continue;
        const char *needs = HERMES_IMPORT_RULES[r].needs;
        if (strcmp(needs, "@scheduler") == 0) {
            if (schedulers == 0) {
                fprintf(stderr, "hepyc: 'import %s' needs a scheduler: add 'import os_priority'\n",
                        HERMES_IMPORT_RULES[r].import);
                ok = 0;
            }
        } else if (!has_import(imports, n, needs)) {
            fprintf(stderr, "hepyc: 'import %s' needs 'import %s' (write it yourself: hepyc never adds it)\n",
                    HERMES_IMPORT_RULES[r].import, needs);
            ok = 0;
        }
    }
    if (schedulers > 1) {
        fprintf(stderr, "hepyc: at most one scheduler per program\n");
        ok = 0;
    }
    return ok;
}

int hermes_corelib_needs_ctx(void) {
    for (int i = 0; i < g_selected_count; i++) {
        if (g_selected[i]->needs_ctx) return 1;
    }
    return 0;
}

char *hermes_corelib_source_for_platform(const char *source, const char *platform) {
    static const char KEY[] = "{platform}";
    size_t n = 0;
    for (const char *p = strstr(source, KEY); p; p = strstr(p + sizeof(KEY) - 1, KEY)) n++;
    char *out = malloc(strlen(source) + n * strlen(platform) + 1), *o = out;
    for (const char *p = source;;) {
        const char *q = strstr(p, KEY);
        if (!q) { strcpy(o, p); break; }
        memcpy(o, p, (size_t)(q - p)); o += q - p;
        strcpy(o, platform); o += strlen(platform);
        p = q + sizeof(KEY) - 1;
    }
    return out;
}
