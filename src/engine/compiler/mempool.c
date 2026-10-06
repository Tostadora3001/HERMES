#include "mempool.h"
#include <string.h>

/* Starting presets, tied to real chip classes rather than arbitrary round
 * numbers -- see docs/raw/hermes_mempool.en.md for the reasoning. Anyone
 * needing an exact value outside these three can bypass the presets
 * entirely by passing -DHERMES_POOL_BYTES=<N> to the build themselves;
 * these are just convenient, named defaults for that same flag. */
const HermesMemPoolProfile HERMES_MEMPOOL_PROFILES[] = {
    { "light",  1024  },  /* tiny chips, e.g. ATtiny-class, <=1-2 KB RAM total */
    { "medium", 4096  },  /* mid-range, e.g. ATmega2560-class, ~8 KB RAM */
    { "heavy",  16384 },  /* roomier parts, e.g. small ARM Cortex-M, tens of KB RAM */
};
const int HERMES_MEMPOOL_PROFILE_COUNT =
    sizeof(HERMES_MEMPOOL_PROFILES) / sizeof(HERMES_MEMPOOL_PROFILES[0]);

const HermesMemPoolProfile *hermes_mempool_lookup(const char *import_name) {
    for (int i = 0; i < HERMES_MEMPOOL_PROFILE_COUNT; i++) {
        if (strcmp(HERMES_MEMPOOL_PROFILES[i].import_name, import_name) == 0) {
            return &HERMES_MEMPOOL_PROFILES[i];
        }
    }
    return NULL;
}
