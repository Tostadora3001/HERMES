#!/usr/bin/env bash
# Examples of hermes_stdlib (list in scripts/examples_lib.sh). Everything: scripts/Compilation_Execution_test.sh
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/examples_lib.sh"

hermes_run_libraries hermes_stdlib
hermes_summary
