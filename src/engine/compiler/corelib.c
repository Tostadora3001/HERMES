#include "corelib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const HermesCoreLib HERMES_CORELIBS[] = {
    /* internal: everything a thread or a device needs at runtime -- the
     * thread library (cores, run/launch/wait), semaphores, mutexes,
     * hermes_runtime_alloc (in hermes_thread.c) and the device registry. */
    { "@runtime",
      { "framework/core/hermes_thread.c", "framework/core/hermes_sem.c",
        "framework/core/hermes_mutex.c",
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
    { "driver:adc",   { "framework/drivers/hermes_drv_adc.c",
                        "framework/drivers/{platform}/hermes_drv_adc_{platform}.c", NULL },   { NULL }, 0 },
    { "driver:pwm",   { "framework/drivers/hermes_drv_pwm.c",
                        "framework/drivers/{platform}/hermes_drv_pwm_{platform}.c", NULL },
                      { "driver:gpio", "driver:timers", NULL }, 0 },
    { "driver:spi",   { "framework/drivers/hermes_drv_spi.c",
                        "framework/drivers/{platform}/hermes_drv_spi_{platform}.c", NULL },
                      { "driver:gpio", NULL }, 0 }, /* chip selects, SS */
    { "driver:i2c",   { "framework/drivers/hermes_drv_i2c.c",
                        "framework/drivers/{platform}/hermes_drv_i2c_{platform}.c", NULL },
                      { "driver:gpio", NULL }, 0 }, /* SDA/SCL pull-ups */
    /* who uses each timer (roles): common code only, no platform part */
    { "driver:timers", { "framework/drivers/hermes_drv_timers.c", NULL }, { NULL }, 0 },

    /* importable libraries */
    { "hermes_threads", { NULL }, { "@runtime", NULL }, 0 },
    { "hermes_parallel", { "framework/core/hermes_parallel.c", NULL }, { "@runtime", NULL }, 0 },

    /* the mini OS and its schedulers: what they need from each other is
     * NOT pulled in automatically -- the user must write every import
     * (decided with the user, so the program states what it contains);
     * see hermes_corelib_check_rules */
    /* hermes_inherit.c: priority inheritance -- priorities only exist under
     * the OS (weak references from the runtime, hermes_sched_internal.h) */
    { "hermes_os",   { "framework/core/hermes_os.c", "framework/core/hermes_inherit.c", NULL },   { "@runtime", "driver:timer", NULL }, 0 },
    { "os_priority", { "framework/core/os_priority.c", NULL }, { NULL }, 0 },
    { "os_deadline", { "framework/core/os_deadline.c", NULL }, { NULL }, 0 },
    { "os_cyclic",   { "framework/core/os_cyclic.c", NULL },   { NULL }, 0 },
    { "hermes_devices", { NULL }, { "@runtime", NULL }, 0 },
    { "hermes_time",    { NULL }, { "driver:timer", NULL }, 0 },
    { "hermes_uart",    { NULL }, { "driver:uart", "@runtime", NULL }, 0 },
    /* communication protocols: one import each (I2C first) */
    { "hermes_i2c",     { NULL }, { "driver:i2c", "@runtime", NULL }, 0 },
    { "hermes_spi",     { NULL }, { "driver:spi", "@runtime", NULL }, 0 },
    /* TCP/UDP sockets over an interface (hermes_net.h); the host's own
     * sockets (net_host) in hermes_net_host.c, empty on a board */
    { "hermes_net",     { "framework/devices/hermes_net.c", "framework/devices/hermes_net_host.c", NULL },
                        { "driver:timer", "@runtime", NULL }, 0 },
    /* reliable packets over a serial port (its own .c: the protocol) */
    { "hermes_packet",  { "framework/devices/hermes_packet.c", NULL },
                        { "driver:uart", "driver:timer", "@runtime", NULL }, 0 },
    { "hermes_adc",     { NULL }, { "driver:adc", "@runtime", NULL }, 0 },
    { "hermes_pwm",     { NULL }, { "driver:pwm", "@runtime", NULL }, 0 },
    /* driver:timers comes with driver:pwm */
    { "hermes_timers",  { NULL }, { "driver:timer", "driver:pwm", "@runtime", NULL }, 0 },
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
    { "os_deadline", "hermes_os" },
    { "os_cyclic",   "hermes_os" },
};
static const char *const HERMES_SCHEDULERS[] = { "os_priority", "os_deadline", "os_cyclic", NULL };

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
                fprintf(stderr, "hepyc: 'import %s' needs a scheduler: add one of 'import os_priority', "
                        "'import os_deadline' or 'import os_cyclic'\n",
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
