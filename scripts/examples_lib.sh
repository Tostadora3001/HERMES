#!/usr/bin/env bash
# Shared by every example test script (loaded with `source`):
#   scripts/Compilation_Execution_test.sh   every library
#   scripts/test_hermes_<library>.sh        only one library's examples
#
# For each example: compile it with hepyc, run the host binary and compare
# its output with tests/expected/<example>.out (the same file ctest uses).
# If the example imports `avr` (multi-architecture), also build and run
# its variant for every board -- Uno and Mega under simavr, ESP32-C3 in
# Espressif's QEMU -- and compare what comes out of the UART with the
# expected output (scripts/board_lib.sh: the variants, the
# `# skip <board>:` / `# sed <board>:` / `# simavr_bench:` lines and which
# expected file each board uses). The test bench (scripts/simavr_bench:
# simavr plus I2C chips, wires between UARTs...)
# is built here if libsimavr-dev is installed. A board whose tools are
# missing is skipped. A failure doesn't stop the rest: they are all
# summarized at the end.

# --- which examples each library tests ------------------------------------
# An example belongs to the library it demonstrates (almost all of them
# import hermes_stdlib for print, but only the basic ones are "stdlib"
# examples). WHEN ADDING AN EXAMPLE: put it here, in tests/CMakeLists.txt,
# and create its tests/expected/<example>.out.
HERMES_LIBRARIES=(hermes_stdlib hermes_threads hermes_parallel hermes_devices hermes_drivers hermes_os hermes_comm)
declare -A HERMES_LIBRARY_EXAMPLES=(
    [hermes_stdlib]="hello factorial fizzbuzz insert_c"
    [hermes_threads]="threads_auto threads_sem threads_multiarch threads_cores threads_light"
    [hermes_parallel]="parallel_squares parallel_sum"
    [hermes_devices]="devices_demo devices_registry abort_demo devices_lifecycle devices_handles"
    [hermes_drivers]="drivers_demo drivers_analog timers_roles i2c_master i2c_slave"
    [hermes_comm]="comm_uart_ports comm_packets comm_net comm_net_services comm_net_host comm_wifi comm_bluetooth"
    [hermes_os]="os_priority_demo os_cores os_tick os_preempt os_sleep os_mutex os_debounce os_deadline os_cyclic os_slice os_feasible os_inherit os_share"
)

# Output whose order isn't deterministic (threads on several cores at
# once): compared sorted. The mocks' lines ([gpio mock], [pwm mock]...) exist only on the host, and
# in some examples their count depends on timing: never compared.
HERMES_UNORDERED_EXAMPLES=" parallel_squares "

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=board_lib.sh
source "$SCRIPT_DIR/board_lib.sh"
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
    # the I2C test bench (needs libsimavr-dev; without it, the examples
    # that need it skip their AVR variant)
    rm -f "$BUILD_DIR/simavr_bench"
    if [ -f /usr/include/simavr/sim_avr.h ]; then
        gcc -O1 -Wall -I/usr/include/simavr "$SCRIPT_DIR/simavr_bench/simavr_bench.c" \
            "$ROOT_DIR/modules/w5500/w5500_virtual.c" "$ROOT_DIR/modules/esp01/esp01_virtual.c" \
            "$ROOT_DIR/modules/hc05/bt_virtual.c" \
            -lsimavr -lsimavrparts -o "$BUILD_DIR/simavr_bench" || echo "(simavr_bench not built)"
    fi
}

# comparison filter for example $1 (reads stdin)
hermes_compare_filter() {
    { grep -v '^\[[a-z]* mock\]' || true; } \
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
    hermes_with_services "$ROOT_DIR" "$EXAMPLES_DIR/$name.hepy" "$bin" < /dev/null | tee "$BUILD_DIR/${name}_native.out" || rc=$?
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

    # multi-architecture: also on every board (scripts/board_lib.sh)
    grep -qx 'import avr' "$EXAMPLES_DIR/$name.hepy" || return 0
    local board rc ref
    for board in $HERMES_BOARDS; do
        echo "---- output of ${name} on $board ----"
        rc=0
        hermes_board_run "$BUILD_DIR/hepyc" "$ROOT_DIR" "$EXAMPLES_DIR/$name.hepy" "$BUILD_DIR" "$board" \
            "$BUILD_DIR/simavr_bench" || rc=$?
        if [ "$rc" = 77 ]; then continue; fi
        if [ "$rc" != 0 ]; then
            HERMES_FAILED+=("$name ($board variant doesn't compile)")
            continue
        fi
        cat "$BUILD_DIR/${name}_$board.out"
        ref=$(hermes_board_expected "$EXPECTED_DIR" "$name" "$board") || ref="$BUILD_DIR/${name}_native.out"
        if diff <(hermes_compare_filter "$name" < "$ref") \
                <(hermes_compare_filter "$name" < "$BUILD_DIR/${name}_$board.out") > /dev/null; then
            echo "---- $board == ${ref#$ROOT_DIR/}: OK ----"
        else
            echo "---- $board != ${ref#$ROOT_DIR/}: DIFFERENT ----"
            HERMES_FAILED+=("$name ($board != ${ref##*/})")
        fi
    done
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
