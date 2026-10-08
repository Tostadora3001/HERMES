#!/usr/bin/env bash
# Compila hepyc (si es necesario), transpila y compila los ejemplos de
# examples/, y ejecuta los binarios resultantes mostrando su salida.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
COMPILER_DIR="$ROOT_DIR/src/engine/compiler"
EXAMPLES_DIR="$ROOT_DIR/examples"

mkdir -p "$BUILD_DIR"

echo "== Compilando hepyc =="
gcc -std=c11 -Wall -Wextra -I"$COMPILER_DIR" \
    "$COMPILER_DIR/main.c" "$COMPILER_DIR/lexer.c" \
    "$COMPILER_DIR/ast.c" "$COMPILER_DIR/parser.c" \
    "$COMPILER_DIR/codegen.c" "$COMPILER_DIR/arch.c" "$COMPILER_DIR/mempool.c" \
    "$COMPILER_DIR/devicekind.c" "$COMPILER_DIR/module.c" "$COMPILER_DIR/corelib.c" \
    "$COMPILER_DIR/repo.c" -o "$BUILD_DIR/hepyc"

EXAMPLES=(hello factorial fizzbuzz threads_auto threads_sem threads_multiarch parallel_squares threads_pool_light devices_demo devices_registry drivers_demo abort_demo)
MULTIARCH_EXAMPLES=(threads_multiarch parallel_squares threads_pool_light devices_demo devices_registry drivers_demo abort_demo)

FAILED=()

for name in "${EXAMPLES[@]}"; do
    echo
    echo "############################"
    echo "## $name.hepy"
    echo "############################"

    # Sin --src/-I/--core-dir: hepyc enlaza e incluye por si mismo lo que
    # pide cada import (runtime, drivers, modulos; ver corelib.c).
    "$BUILD_DIR/hepyc" "$EXAMPLES_DIR/$name.hepy" \
        -o "$BUILD_DIR/$name.c" \
        --compile "$BUILD_DIR/$name"

    is_multiarch=0
    for m in "${MULTIARCH_EXAMPLES[@]}"; do [ "$m" = "$name" ] && is_multiarch=1; done

    if [ "$is_multiarch" = "1" ]; then
        echo "---- salida de build/${name}_native ----"
        "$BUILD_DIR/${name}_native" | tee "$BUILD_DIR/${name}_native.out"
        if command -v simavr >/dev/null 2>&1; then
            # Variante solo-AVR del mismo programa: print() por la UART real
            # (import hermes_uart) y fin limpio de la simulacion
            # (avr_sim_exit.c), para comparar su salida con la nativa.
            sed 's/^import native$/import hermes_uart/' "$EXAMPLES_DIR/$name.hepy" > "$BUILD_DIR/${name}_avrtest.hepy"
            "$BUILD_DIR/hepyc" "$BUILD_DIR/${name}_avrtest.hepy" \
                -o "$BUILD_DIR/${name}_avrtest.c" --compile "$BUILD_DIR/${name}_avrtest.elf" \
                --src "$SCRIPT_DIR/avr_sim_exit.c" > /dev/null
            echo "---- salida de ${name} en AVR (simavr, UART) ----"
            timeout 10 simavr -m atmega328p -f 16000000 "$BUILD_DIR/${name}_avrtest.elf" 2>&1 \
                | sed 's/\x1b\[[0-9;]*m//g; s/\.$//' | grep -v '^Loaded ' \
                | tee "$BUILD_DIR/${name}_avr.out" || true
            # El mock de gpio solo existe en host; parallel_squares no tiene
            # orden determinista en host (hilos reales del SO).
            filter() { grep -v '^\[gpio mock\]' "$1" | { [ "$name" = parallel_squares ] && sort -n || cat; }; }
            if diff <(filter "$BUILD_DIR/${name}_native.out") <(filter "$BUILD_DIR/${name}_avr.out") > /dev/null; then
                echo "---- AVR == native: OK ----"
            else
                echo "---- AVR != native: DIFERENTE ----"
                FAILED+=("$name (AVR != native)")
            fi
        else
            echo "---- simavr no disponible: no se ejecuta la variante AVR ----"
        fi
    else
        echo "---- salida de build/$name ----"
        "$BUILD_DIR/$name"
    fi
done

echo
if [ "${#FAILED[@]}" -eq 0 ]; then
    echo "== Todo OK =="
else
    echo "== FALLOS: ${FAILED[*]} =="
    exit 1
fi
