#!/usr/bin/env bash
# Shared by every example test script (loaded with `source`):
#   scripts/Compilation_Execution_test.sh   every library
#   scripts/test_hermes_<library>.sh        only one library's examples
#
# For each example: compile it with hepyc, run the host binary and compare
# its output with tests/expected/<example>.out (the same file ctest uses).
# If the example imports `avr` and simavr is available, also build the
# AVR-only variant (print through the UART: 'import native' -> 'import
# hermes_uart', + avr_sim_exit.c to end the simulation), run it under
# simavr and compare its output with the host's -- or with
# tests/expected/<example>.avr.out if it exists (an example whose output
# differs on bare metal on purpose, e.g. preemption). A failure doesn't
# stop the rest: they are all summarized at the end.

# --- which examples each library tests ------------------------------------
# An example belongs to the library it demonstrates (almost all of them
# import hermes_stdlib for print, but only the basic ones are "stdlib"
# examples). WHEN ADDING AN EXAMPLE: put it here, in tests/CMakeLists.txt,
# and create its tests/expected/<example>.out.
HERMES_LIBRARIES=(hermes_stdlib hermes_threads hermes_parallel hermes_devices hermes_drivers hermes_os)
declare -A HERMES_LIBRARY_EXAMPLES=(
    [hermes_stdlib]="hello factorial fizzbuzz insert_c"
    [hermes_threads]="threads_auto threads_sem threads_multiarch threads_cores threads_light"
    [hermes_parallel]="parallel_squares parallel_sum"
    [hermes_devices]="devices_demo devices_registry abort_demo"
    [hermes_drivers]="drivers_demo"
    [hermes_os]="os_priority_demo os_cores os_tick os_preempt"
)

# Output whose order isn't deterministic (threads on several cores at
# once): compared sorted. The gpio mock's lines exist only on the host, and
# in some examples their count depends on timing: never compared.
HERMES_UNORDERED_EXAMPLES=" parallel_squares "

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
COMPILER_DIR="$ROOT_DIR/src/engine/compiler"
EXAMPLES_DIR="$ROOT_DIR/examples"
EXPECTED_DIR="$ROOT_DIR/tests/expected"

HERMES_FAILED=()

hermes_build_hepyc() {
    mkdir -p "$BUILD_DIR"
    echo "== Building hepyc =="
    gcc -std=c11 -Wall -Wextra -I"$COMPILER_DIR" \
        "$COMPILER_DIR/main.c" "$COMPILER_DIR/lexer.c" \
        "$COMPILER_DIR/ast.c" "$COMPILER_DIR/parser.c" \
        "$COMPILER_DIR/codegen.c" "$COMPILER_DIR/arch.c" "$COMPILER_DIR/board.c" "$COMPILER_DIR/threadprofile.c" \
        "$COMPILER_DIR/devicekind.c" "$COMPILER_DIR/module.c" "$COMPILER_DIR/corelib.c" \
        "$COMPILER_DIR/repo.c" "$COMPILER_DIR/diagnostics.c" -o "$BUILD_DIR/hepyc"
}

# comparison filter for example $1 (reads stdin)
hermes_compare_filter() {
    { grep -v '^\[gpio mock\]' || true; } \
        | if [[ "$HERMES_UNORDERED_EXAMPLES" == *" $1 "* ]]; then sort -n; else cat; fi
}

hermes_run_example() {
    local name=$1
    echo
    echo "############################"
    echo "## $name.hepy"
    echo "############################"

    if ! "$BUILD_DIR/hepyc" "$EXAMPLES_DIR/$name.hepy" -o "$BUILD_DIR/$name.c" --compile "$BUILD_DIR/$name"; then
        HERMES_FAILED+=("$name (doesn't compile)")
        return
    fi

    local bin="$BUILD_DIR/$name"
    [ -x "${bin}_native" ] && bin="${bin}_native"
    echo "---- output of ${bin#$ROOT_DIR/} ----"
    local rc=0
    "$bin" < /dev/null | tee "$BUILD_DIR/${name}_native.out" || rc=$?
    if [ "$rc" != 0 ]; then
        HERMES_FAILED+=("$name (exits with code $rc)")
    elif [ -f "$EXPECTED_DIR/$name.out" ]; then
        if diff <(hermes_compare_filter "$name" < "$BUILD_DIR/${name}_native.out") \
                <(hermes_compare_filter "$name" < "$EXPECTED_DIR/$name.out") > /dev/null; then
            echo "---- == tests/expected/$name.out: OK ----"
        else
            echo "---- != tests/expected/$name.out: DIFFERENT ----"
            HERMES_FAILED+=("$name (output != expected)")
        fi
    else
        echo "---- no tests/expected/$name.out: not compared ----"
    fi

    grep -qx 'import avr' "$EXAMPLES_DIR/$name.hepy" || return 0
    if ! command -v simavr > /dev/null 2>&1; then
        echo "---- simavr not available: AVR variant not run ----"
        return
    fi
    sed 's/^import native$/import hermes_uart/' "$EXAMPLES_DIR/$name.hepy" > "$BUILD_DIR/${name}_avrtest.hepy"
    if ! "$BUILD_DIR/hepyc" "$BUILD_DIR/${name}_avrtest.hepy" \
            -o "$BUILD_DIR/${name}_avrtest.c" --compile "$BUILD_DIR/${name}_avrtest.elf" \
            --src "$SCRIPT_DIR/avr_sim_exit.c" > /dev/null; then
        HERMES_FAILED+=("$name (AVR variant doesn't compile)")
        return
    fi
    echo "---- output of ${name} on AVR (simavr, UART) ----"
    { timeout 10 simavr -m atmega328p -f 16000000 "$BUILD_DIR/${name}_avrtest.elf" 2>&1 || true; } \
        | sed 's/\x1b\[[0-9;]*m//g; s/\.$//' | { grep -v '^Loaded ' || true; } \
        | tee "$BUILD_DIR/${name}_avr.out"
    local ref="$BUILD_DIR/${name}_native.out" what=native
    if [ -f "$EXPECTED_DIR/$name.avr.out" ]; then
        ref="$EXPECTED_DIR/$name.avr.out" what="tests/expected/$name.avr.out"
    fi
    if diff <(hermes_compare_filter "$name" < "$ref") \
            <(hermes_compare_filter "$name" < "$BUILD_DIR/${name}_avr.out") > /dev/null; then
        echo "---- AVR == $what: OK ----"
    else
        echo "---- AVR != $what: DIFFERENT ----"
        HERMES_FAILED+=("$name (AVR != $what)")
    fi
}

# Tests the examples of the given libraries (names from HERMES_LIBRARIES).
hermes_run_libraries() {
    local lib
    for lib in "$@"; do
        if [ -z "${HERMES_LIBRARY_EXAMPLES[$lib]+x}" ]; then
            echo "unknown library: $lib (known: ${HERMES_LIBRARIES[*]})" >&2
            exit 2
        fi
    done
    hermes_build_hepyc
    for lib in "$@"; do
        echo
        echo "======== $lib ========"
        local name
        for name in ${HERMES_LIBRARY_EXAMPLES[$lib]}; do hermes_run_example "$name"; done
    done
}

# Warns about examples in examples/ that are in no list.
hermes_check_unlisted() {
    local f name lib found
    for f in "$EXAMPLES_DIR"/*.hepy; do
        name=$(basename "$f" .hepy)
        found=0
        for lib in "${HERMES_LIBRARIES[@]}"; do
            [[ " ${HERMES_LIBRARY_EXAMPLES[$lib]} " == *" $name "* ]] && found=1
        done
        [ "$found" = 1 ] || echo "WARNING: examples/$name.hepy is in no library (scripts/examples_lib.sh): not tested"
    done
}

hermes_summary() {
    echo
    if [ "${#HERMES_FAILED[@]}" -eq 0 ]; then
        echo "== All OK =="
    else
        echo "== FAILURES: =="
        printf '   %s\n' "${HERMES_FAILED[@]}"
        exit 1
    fi
}
