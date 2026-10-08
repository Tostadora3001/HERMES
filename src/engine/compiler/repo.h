#ifndef HERMES_REPO_H
#define HERMES_REPO_H

/* Root of the HERMES repo, where hepyc finds everything it links or
 * includes on its own: src/ (framework + useful) and modules/.
 *
 * Resolution order: an explicit `--repo-dir` (passed in as `override`);
 * else HERMES_DEFAULT_REPO_DIR if hepyc was built with it (CMake sets it:
 * its hepyc doesn't live directly in build/); else the parent of the
 * folder holding the hepyc binary (build/hepyc -> the repo root).
 * Canonicalized with realpath when it exists. Returns a static buffer. */
const char *hermes_repo_dir(const char *argv0, const char *override);

/* Allocated "<repo>/<sub>" (sub may contain slashes). */
char *hermes_repo_path(const char *repo_dir, const char *sub);

#endif
