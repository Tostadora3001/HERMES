#!/usr/bin/env bash
# Copies one or more generated .c files without their `#line` directives,
# to read the C that hepyc generates more easily. The originals are not
# touched (they need the #line directives: they make the C compiler report
# errors at the .hepy line).
#
#   scripts/clean_c.sh [-o <dir>] <file.c>...
#
#   -o, --out-dir <dir>   where to write the copies (default: build/clean_build)
#
# Each copy keeps the file name of its original. Every line that is a
# `#line` directive is removed (also inside insert_C blocks), nothing else.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/build/clean_build"

usage() {
    echo "usage: $0 [-o <dir>] <file.c>..." >&2
    echo "  -o, --out-dir <dir>   where to write the copies (default: build/clean_build)" >&2
}

FILES=()
while [ $# -gt 0 ]; do
    case "$1" in
        -o|--out-dir)
            [ $# -ge 2 ] || { echo "clean_c: $1 needs a directory" >&2; usage; exit 1; }
            OUT_DIR="$2"
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "clean_c: unknown option: $1" >&2; usage; exit 1 ;;
        *) FILES+=("$1"); shift ;;
    esac
done

[ ${#FILES[@]} -gt 0 ] || { echo "clean_c: no .c file given" >&2; usage; exit 1; }

mkdir -p "$OUT_DIR"
declare -A SEEN=()
status=0
for f in "${FILES[@]}"; do
    if [ ! -f "$f" ]; then
        echo "clean_c: $f: file not found" >&2
        status=1
        continue
    fi
    name="$(basename "$f")"
    if [ -n "${SEEN[$name]:-}" ]; then
        echo "clean_c: $f: same name as ${SEEN[$name]}, the copy overwrites it" >&2
    fi
    SEEN[$name]="$f"
    grep -Ev '^[[:space:]]*#[[:space:]]*line[[:space:]]' "$f" > "$OUT_DIR/$name" || true
    echo "clean_c: $f -> $OUT_DIR/$name"
done
exit $status
