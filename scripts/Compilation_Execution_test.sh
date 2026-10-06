#!/usr/bin/env bash
# Compila hepyc (si es necesario), transpila y compila los ejemplos de
# examples/, y ejecuta los binarios resultantes mostrando su salida.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
COMPILER_DIR="$ROOT_DIR/src/engine/compiler"
USEFUL_DIR="$ROOT_DIR/src/useful"
CORE_DIR="$ROOT_DIR/src/framework/core"
DEVICES_DIR="$ROOT_DIR/src/framework/devices"
EXAMPLES_DIR="$ROOT_DIR/examples"
THREAD_RUNTIME_SRCS=("$CORE_DIR/hermes_thread.c" "$CORE_DIR/hermes_sem.c" "$CORE_DIR/hermes_ctx_x86_64.S")

mkdir -p "$BUILD_DIR"

echo "== Compilando hepyc =="
gcc -std=c11 -Wall -Wextra -I"$COMPILER_DIR" \
    "$COMPILER_DIR/main.c" "$COMPILER_DIR/lexer.c" \
    "$COMPILER_DIR/ast.c" "$COMPILER_DIR/parser.c" \
    "$COMPILER_DIR/codegen.c" "$COMPILER_DIR/arch.c" "$COMPILER_DIR/mempool.c" \
    "$COMPILER_DIR/devicekind.c" -o "$BUILD_DIR/hepyc"

EXAMPLES=(hello factorial fizzbuzz threads_auto threads_sem threads_multiarch parallel_squares threads_pool_light devices_demo)
MULTIARCH_EXAMPLES=(threads_multiarch parallel_squares threads_pool_light devices_demo)

for name in "${EXAMPLES[@]}"; do
    echo
    echo "############################"
    echo "## $name.hepy"
    echo "############################"

    extra_src_args=()
    case "$name" in
        threads_multiarch|threads_pool_light|devices_demo)
            extra_src_args+=(--src "$CORE_DIR/hermes_thread.c" --src "$CORE_DIR/hermes_sem.c" --core-dir "$CORE_DIR")
            ;;
        parallel_squares)
            extra_src_args+=(--core-dir "$CORE_DIR")
            ;;
        threads_*)
            for src in "${THREAD_RUNTIME_SRCS[@]}"; do
                extra_src_args+=(--src "$src")
            done
            ;;
    esac

    "$BUILD_DIR/hepyc" "$EXAMPLES_DIR/$name.hepy" \
        -o "$BUILD_DIR/$name.c" \
        --compile "$BUILD_DIR/$name" \
        -I "$USEFUL_DIR" -I "$CORE_DIR" -I "$DEVICES_DIR" \
        "${extra_src_args[@]}"

    is_multiarch=0
    for m in "${MULTIARCH_EXAMPLES[@]}"; do [ "$m" = "$name" ] && is_multiarch=1; done

    if [ "$is_multiarch" = "1" ]; then
        echo "---- salida de build/${name}_native ----"
        "$BUILD_DIR/${name}_native"
        if command -v simavr >/dev/null 2>&1; then
            echo "---- build/${name}_avr.elf bajo simavr (3s) ----"
            timeout 3 simavr -m atmega328p -f 16000000 "$BUILD_DIR/${name}_avr.elf" || true
        else
            echo "---- build/${name}_avr.elf generado (simavr no disponible para ejecutarlo aqui) ----"
        fi
    else
        echo "---- salida de build/$name ----"
        "$BUILD_DIR/$name"
    fi
done
