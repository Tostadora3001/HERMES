#include "board.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HERMES_MAX_BOARDS 16

static HermesBoard g_boards[HERMES_MAX_BOARDS];
static int g_board_count = 0;

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

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* line 0 = a problem with the whole file (e.g. a missing key) */
static void hboard_fail(const char *path, int line, const char *msg, const char *detail) {
    if (line > 0) fprintf(stderr, "%s:%d: ", path, line);
    else fprintf(stderr, "%s: ", path);
    fprintf(stderr, "%s%s%s%s\n", msg,
            detail ? " '" : "", detail ? detail : "", detail ? "'" : "");
    exit(1);
}

/* --- platforms ------------------------------------------------------------ */

#define HERMES_MAX_PLATFORMS 8
static HermesPlatform g_platforms[HERMES_MAX_PLATFORMS];
static int g_platform_count = 0;

static char *replace_dir(const char *s, const char *dir) {
    static const char KEY[] = "{dir}";
    size_t n = 0;
    for (const char *p = strstr(s, KEY); p; p = strstr(p + sizeof(KEY) - 1, KEY)) n++;
    char *out = malloc(strlen(s) + n * strlen(dir) + 1), *o = out;
    for (const char *p = s;;) {
        const char *q = strstr(p, KEY);
        if (!q) { strcpy(o, p); break; }
        memcpy(o, p, (size_t)(q - p)); o += q - p;
        strcpy(o, dir); o += strlen(dir);
        p = q + sizeof(KEY) - 1;
    }
    return out;
}

const HermesPlatform *hermes_platform_load(const char *drivers_dir, const char *name,
                                           const HermesArchProfile *arch) {
    for (int i = 0; i < g_platform_count; i++) {
        if (strcmp(g_platforms[i].name, name) == 0) return &g_platforms[i];
    }
    if (g_platform_count >= HERMES_MAX_PLATFORMS) {
        fprintf(stderr, "hepyc: too many platforms (at most %d)\n", HERMES_MAX_PLATFORMS);
        exit(1);
    }
    HermesPlatform pl;
    memset(&pl, 0, sizeof(pl));
    pl.name = xstrdup(name);
    pl.dir = path_join(drivers_dir, name);
    pl.cflags = "";

    char *path = path_join(pl.dir, "platform.hplat");
    FILE *f = fopen(path, "r");
    if (!f) {
        /* no description: fine, but the folder (its drivers) must exist */
        char *probe = path_join(pl.dir, ".");
        FILE *d = fopen(probe, "r");
        if (!d) {
            fprintf(stderr, "hepyc: platform '%s' not found (no folder %s)\n", name, pl.dir);
            exit(1);
        }
        fclose(d);
        free(probe);
    } else {
        char raw[1024];
        int line_no = 0, has_name = 0;
        const HermesArchProfile *pl_arch = NULL;
        while (fgets(raw, sizeof(raw), f)) {
            line_no++;
            char *hash = strchr(raw, '#');
            if (hash) *hash = '\0';
            char *line = trim(raw);
            if (!*line) continue;
            char *eq = strchr(line, '=');
            if (!eq) hboard_fail(path, line_no, "expected 'key = value'", NULL);
            *eq = '\0';
            char *key = trim(line);
            char *value = trim(eq + 1);
            if (strcmp(key, "name") == 0) {
                if (strcmp(value, name) != 0) hboard_fail(path, line_no, "'name' must match the platform's folder name", value);
                has_name = 1;
            } else if (strcmp(key, "arch") == 0) {
                pl_arch = hermes_arch_lookup(value);
                if (!pl_arch) hboard_fail(path, line_no, "unknown architecture", value);
            } else if (strcmp(key, "sources") == 0) {
                for (char *tok = strtok(value, " \t"); tok; tok = strtok(NULL, " \t")) {
                    if (pl.num_sources >= HERMES_PLATFORM_MAX_SOURCES) {
                        hboard_fail(path, line_no, "too many sources (at most 8)", NULL);
                    }
                    char *src = path_join(pl.dir, tok);
                    if (!file_exists(src)) hboard_fail(path, line_no, "source file not found:", src);
                    pl.sources[pl.num_sources++] = src;
                }
            } else if (strcmp(key, "cflags") == 0) {
                pl.cflags = replace_dir(value, pl.dir);
            } else {
                hboard_fail(path, line_no, "unknown key", key);
            }
        }
        fclose(f);
        if (!has_name) hboard_fail(path, 0, "missing mandatory key 'name'", NULL);
        if (!pl_arch) hboard_fail(path, 0, "missing mandatory key 'arch'", NULL);
        if (arch && pl_arch != arch) {
            hboard_fail(path, 0, "the platform's architecture doesn't match the board's:", arch->import_name);
        }
    }
    free(path);
    g_platforms[g_platform_count] = pl;
    return &g_platforms[g_platform_count++];
}

/* --- boards --------------------------------------------------------------- */

const HermesBoard *hermes_board_find(const char *name) {
    for (int i = 0; i < g_board_count; i++) {
        if (strcmp(g_boards[i].name, name) == 0) return &g_boards[i];
    }
    return NULL;
}

const HermesBoard *hermes_board_load(const char *boards_dir, const char *drivers_dir,
                                     const char *name) {
    const HermesBoard *existing = hermes_board_find(name);
    if (existing) return existing;

    char *dir = path_join(boards_dir, name);
    char *path = path_join(dir, "board.hboard");
    if (!file_exists(path)) {
        free(dir);
        free(path);
        return NULL;
    }
    if (g_board_count >= HERMES_MAX_BOARDS) {
        fprintf(stderr, "hepyc: too many boards imported (at most %d)\n", HERMES_MAX_BOARDS);
        exit(1);
    }

    HermesBoard b;
    memset(&b, 0, sizeof(b));
    b.name = xstrdup(name);
    b.dir = dir;

    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char raw[1024];
    int line_no = 0, has_name = 0;
    char *platform = NULL;
    while (fgets(raw, sizeof(raw), f)) {
        line_no++;
        char *hash = strchr(raw, '#');
        if (hash) *hash = '\0';
        char *line = trim(raw);
        if (!*line) continue;
        char *eq = strchr(line, '=');
        if (!eq) hboard_fail(path, line_no, "expected 'key = value'", NULL);
        *eq = '\0';
        char *key = trim(line);
        char *value = trim(eq + 1);

        if (strcmp(key, "name") == 0) {
            if (strcmp(value, name) != 0) hboard_fail(path, line_no, "'name' must match the board's folder name", value);
            has_name = 1;
        } else if (strcmp(key, "description") == 0) {
            b.description = xstrdup(value);
        } else if (strcmp(key, "arch") == 0) {
            b.arch = hermes_arch_lookup(value);
            if (!b.arch) hboard_fail(path, line_no, "unknown architecture", value);
            if (!b.arch->mcu_flag) hboard_fail(path, line_no, "this architecture has no boards", value);
        } else if (strcmp(key, "platform") == 0) {
            for (const char *p = value; *p; p++) {
                if (!(isalnum((unsigned char)*p) || *p == '_')) hboard_fail(path, line_no, "invalid platform name", value);
            }
            platform = xstrdup(value);
        } else if (strcmp(key, "mcu") == 0) {
            for (const char *p = value; *p; p++) {
                if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) {
                    hboard_fail(path, line_no, "invalid chip name", value);
                }
            }
            b.mcu = xstrdup(value);
        } else if (strcmp(key, "f_cpu") == 0) {
            if (!*value) hboard_fail(path, line_no, "f_cpu must be a frequency in Hz", value);
            for (const char *p = value; *p; p++) {
                if (!isdigit((unsigned char)*p)) hboard_fail(path, line_no, "f_cpu must be a frequency in Hz", value);
            }
            b.f_cpu = xstrdup(value);
        } else {
            hboard_fail(path, line_no, "unknown key", key);
        }
    }
    fclose(f);

    if (!has_name) hboard_fail(path, 0, "missing mandatory key 'name'", NULL);
    if (!b.arch) hboard_fail(path, 0, "missing mandatory key 'arch'", NULL);
    if (!b.mcu) hboard_fail(path, 0, "missing mandatory key 'mcu'", NULL);
    if (!b.f_cpu) hboard_fail(path, 0, "missing mandatory key 'f_cpu'", NULL);
    char *header = path_join(dir, "hermes_board.h");
    if (!file_exists(header)) hboard_fail(path, 0, "board header not found:", header);
    free(header);
    b.platform = hermes_platform_load(drivers_dir, platform ? platform : b.arch->import_name, b.arch);
    free(path);

    g_boards[g_board_count] = b;
    return &g_boards[g_board_count++];
}
