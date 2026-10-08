#include "corelib.h"
#include <string.h>

static const HermesCoreLib HERMES_CORELIBS[] = {
    /* internal: everything a thread or a device needs at runtime -- the
     * cooperative scheduler, semaphores, hermes_runtime_alloc (in
     * hermes_thread.c) and the device registry. */
    { "@runtime",
      { "framework/core/hermes_thread.c", "framework/core/hermes_sem.c",
        "framework/devices/hermes_device.c", NULL },
      { NULL }, 1 },

    /* drivers (src/framework/drivers) */
    { "driver:gpio",  { "framework/drivers/hermes_drv_gpio.c", NULL },  { NULL }, 0 },
    { "driver:timer", { "framework/drivers/hermes_drv_timer.c", NULL }, { NULL }, 0 },
    { "driver:uart",  { "framework/drivers/hermes_drv_uart.c", NULL },  { NULL }, 0 },

    /* importable libraries */
    { "hermes_threads", { NULL }, { "@runtime", NULL }, 0 },
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

const HermesCoreLib *hermes_corelib_selected_at(int i) {
    return (i >= 0 && i < g_selected_count) ? g_selected[i] : NULL;
}

int hermes_corelib_needs_ctx(void) {
    for (int i = 0; i < g_selected_count; i++) {
        if (g_selected[i]->needs_ctx) return 1;
    }
    return 0;
}
