#define _XOPEN_SOURCE 700 /* readlink, realpath */
#include "repo.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *hermes_repo_dir(const char *argv0, const char *override) {
    static char buf[PATH_MAX];
    char raw[PATH_MAX + 16];

    if (override) {
        snprintf(raw, sizeof(raw), "%s", override);
    } else {
#ifdef HERMES_DEFAULT_REPO_DIR
        snprintf(raw, sizeof(raw), "%s", HERMES_DEFAULT_REPO_DIR);
        (void)argv0;
#else
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) exe[n] = '\0';
        else snprintf(exe, sizeof(exe), "%s", argv0);
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(raw, sizeof(raw), "%s/..", exe);
        } else {
            snprintf(raw, sizeof(raw), ".."); /* argv0 without a path: best effort */
        }
#endif
    }
    if (!realpath(raw, buf)) snprintf(buf, sizeof(buf), "%.*s", (int)sizeof(buf) - 1, raw);
    return buf;
}

char *hermes_repo_path(const char *repo_dir, const char *sub) {
    size_t n = strlen(repo_dir) + 1 + strlen(sub) + 1;
    char *p = malloc(n);
    snprintf(p, n, "%s/%s", repo_dir, sub);
    return p;
}
