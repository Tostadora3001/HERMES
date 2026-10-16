# Board variants of a multi-architecture example (one that imports native
# and avr): build it for a board, run it in its simulator and pick the
# expected output. Sourced by scripts/examples_lib.sh (the suite) and
# tests/run_hepy_test.sh (ctest), so both test the same way.
#
# Boards (HERMES_BOARDS):
#   uno   ATmega328P, simavr                  'import avr' kept
#   mega  ATmega2560, simavr -m atmega2560    'import avr' -> 'import mega'
#   c3    ESP32-C3, Espressif's QEMU          'import avr' -> 'import esp32c3'
# Each variant: 'import native' -> 'import hermes_uart' (print() through
# the UART) + the simulator's clean exit (scripts/avr_sim_exit.c or
# scripts/esp32c3_sim_exit.c). Its output: what came out of the UART.
#
# Lines an example may carry:
#   # skip <board>: <reason>    not tested on that board (reported as
#                               skipped, with the reason)
#   # sed <board>: <expression> applied to the source of that board's
#                               variant (e.g. other pins); several allowed
#   # echo_server: <port>       an echo server runs meanwhile (see
#                               hermes_with_services)
#   # simavr_bench <board>: <args>  the same for one board (first)
#   # simavr_bench: <args>      on uno/mega, run in the test bench
#                               (scripts/simavr_bench: I2C chips, wires
#                               between UARTs...) with those arguments;
#                               its "[wire] ..." reports (faults injected
#                               on purpose) aren't compared
# Expected output: tests/expected/<example>.<board>.out if it exists (a
# board whose output differs on purpose), else <example>.avr.out (bare
# metal differs from the host on purpose: preemption, warnings through the
# UART), else <example>.out (the host's).

HERMES_BOARDS="uno mega c3"

# hermes_with_services <repo> <example.hepy> <command...>
# Runs the command with the services the example asks for running:
#   # echo_server: PORT   scripts/echo_server.py on 127.0.0.1:PORT (TCP and
#                         UDP echo) -- for the network examples
# One test at a time per port (flock: ctest -j runs the host and the
# boards at once). Returns the command's status; 77 (skip) without python3.
hermes_with_services() {
    local repo=$1 src=$2
    shift 2
    local port
    port=$(sed -n 's/^# echo_server: //p' "$src" | head -n 1)
    if [ -z "$port" ]; then "$@"; return $?; fi
    command -v python3 > /dev/null 2>&1 || { echo "skipped: no python3 for the echo server"; return 77; }
    (
        flock 9
        local log
        log=$(mktemp)
        python3 "$repo/scripts/echo_server.py" "$port" 120 > "$log" 2>&1 &
        local pid=$! i
        for i in $(seq 50); do grep -q ready "$log" && break; sleep 0.1; done
        local rc=0
        "$@" || rc=$?
        kill "$pid" 2> /dev/null
        wait "$pid" 2> /dev/null
        rm -f "$log"
        exit "$rc"
    ) 9> "${TMPDIR:-/tmp}/hermes_echo_$port.lock"
}
HERMES_QEMU_ESP=${HERMES_QEMU_ESP:-qemu-system-riscv32-esp}

# hermes_board_expected <expected_dir> <example> <board>: the file to compare with
hermes_board_expected() {
    local f
    for f in "$1/$2.$3.out" "$1/$2.avr.out" "$1/$2.out"; do
        if [ -f "$f" ]; then echo "$f"; return 0; fi
    done
    return 1
}

# hermes_board_run <hepyc> <repo> <example.hepy> <work_dir> <board> [i2c_bench]
# Builds and runs the variant; its output goes to <work_dir>/<example>_<board>.out.
# Returns 0 if it ran, 77 if skipped (the reason on stdout), 1 if it
# doesn't compile (the log's path on stdout).
hermes_board_run() {
    local hepyc=$1 repo=$2 src=$3 work=$4 board=$5 bench=${6:-}
    local name reason import exitc
    name=$(basename "$src" .hepy)
    reason=$(sed -n "s/^# skip $board: //p" "$src" | head -n 1)
    if [ -n "$reason" ]; then echo "skipped on $board: $reason"; return 77; fi
    case $board in
        uno)  import=avr;     exitc=avr_sim_exit.c ;;
        mega) import=mega;    exitc=avr_sim_exit.c ;;
        c3)   import=esp32c3; exitc=esp32c3_sim_exit.c ;;
        *) echo "unknown board: $board"; return 1 ;;
    esac
    if [ "$board" = c3 ]; then
        command -v riscv64-unknown-elf-gcc > /dev/null 2>&1 && command -v "$HERMES_QEMU_ESP" > /dev/null 2>&1 \
            || { echo "skipped on c3: no riscv64-unknown-elf-gcc or $HERMES_QEMU_ESP"; return 77; }
    else
        command -v avr-gcc > /dev/null 2>&1 && command -v simavr > /dev/null 2>&1 \
            || { echo "skipped on $board: no avr-gcc or simavr"; return 77; }
    fi
    local args=(-e 's/^import native$/import hermes_uart/' -e "s/^import avr\$/import $import/")
    local expr
    while IFS= read -r expr; do
        [ -n "$expr" ] && args+=(-e "$expr")
    done < <(sed -n "s/^# sed $board: //p" "$src")
    mkdir -p "$work"
    local v="$work/${name}_$board"
    sed "${args[@]}" "$src" > "$v.hepy"
    if ! "$hepyc" "$v.hepy" -o "$v.c" --compile "$v.elf" --src "$repo/scripts/$exitc" > "$v.log" 2>&1; then
        echo "doesn't compile on $board: see $v.log"
        return 1
    fi
    if [ "$board" = c3 ]; then
        riscv64-unknown-elf-objcopy -O binary "$v.elf" "$v.bin" && truncate -s 4M "$v.bin"
        # the first 3 lines are the ROM's boot messages
        { timeout 60 "$HERMES_QEMU_ESP" -nographic -machine esp32c3 \
              -drive "file=$v.bin,if=mtd,format=raw" -semihosting-config enable=on,target=native \
              2> /dev/null || true; } | tr -d '\r' | tail -n +4 > "$v.out"
        return 0
    fi
    local mcu=atmega328p
    [ "$board" = mega ] && mcu=atmega2560
    local sim=(simavr -m "$mcu" -f 16000000)
    local benchargs
    benchargs=$(sed -n "s/^# simavr_bench $board: //p" "$src")   # this board's, if any
    [ -n "$benchargs" ] || benchargs=$(sed -n 's/^# simavr_bench: //p' "$src")
    if [ -n "$benchargs" ]; then
        [ -n "$bench" ] && [ -x "$bench" ] || { echo "skipped on $board: no test bench (scripts/simavr_bench: needs libsimavr-dev)"; return 77; }
        eval "sim=(\"\$bench\" -m $mcu -f 16000000 $benchargs)"
    fi
    { hermes_with_services "$repo" "$src" timeout 60 "${sim[@]}" "$v.elf" 2>&1 || true; } \
        | sed 's/\x1b\[[0-9;]*m//g; s/\.$//' | { grep -v '^Loaded \|^\[wire\] ' || true; } > "$v.out"
    return 0
}
