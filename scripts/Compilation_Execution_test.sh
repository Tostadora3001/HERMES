#!/usr/bin/env bash
# Builds hepyc, then builds and runs every example in examples/, comparing
# its output with tests/expected/ and, for those also built for AVR, the
# simavr output with the host's. For a single library:
# scripts/test_hermes_<library>.sh. Shared logic: scripts/examples_lib.sh.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/examples_lib.sh"

hermes_run_libraries "${HERMES_LIBRARIES[@]}"
hermes_check_unlisted
hermes_summary
