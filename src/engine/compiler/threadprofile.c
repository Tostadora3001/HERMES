#include "threadprofile.h"
#include <string.h>

/* The sizes of each level, per target, live in hermes_thread.h
 * (HERMES_THREAD_PROFILE). `medium` is also the default with no profile. */
const HermesThreadProfile HERMES_THREAD_PROFILES[] = {
    { "light",  1 },
    { "medium", 2 },
    { "heavy",  3 },
};
const int HERMES_THREAD_PROFILE_COUNT =
    sizeof(HERMES_THREAD_PROFILES) / sizeof(HERMES_THREAD_PROFILES[0]);

const HermesThreadProfile *hermes_threadprofile_lookup(const char *import_name) {
    for (int i = 0; i < HERMES_THREAD_PROFILE_COUNT; i++) {
        if (strcmp(HERMES_THREAD_PROFILES[i].import_name, import_name) == 0) {
            return &HERMES_THREAD_PROFILES[i];
        }
    }
    return NULL;
}
