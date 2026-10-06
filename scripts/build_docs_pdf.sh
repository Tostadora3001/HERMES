#!/usr/bin/env bash
# Compila cada documento .md de docs/raw a PDF usando la plantilla LaTeX
# "Default" (docs/template/Default.latex) y deja el resultado en
# docs/refined/, siguiendo la estructura prevista en la especificación del
# proyecto (ver CLAUDE.md): raw = fuente, template = plantillas .latex,
# refined = versiones elaboradas (PDF) de los documentos en raw.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RAW_DIR="$ROOT_DIR/docs/raw"
TEMPLATE="$ROOT_DIR/docs/template/Default.latex"
OUT_DIR="$ROOT_DIR/docs/refined"
LOGO="$ROOT_DIR/docs/images/herms_logo.jpeg"

# Idiomas reconocidos por el sufijo del nombre de fichero, p. ej.
# "hermes_devices.en.md" -> inglés, "Definicion.es.md" -> español. El
# nombre a la derecha es el que usa babel dentro de la plantilla
# (docs/template/Default.latex), que selecciona el idioma del documento vía
# la variable "babel-lang" que este script le pasa a pandoc.
#
# Para añadir un idioma nuevo: (1) añadir aquí la entrada "sufijo:nombre_babel",
# y (2) añadir ese mismo nombre_babel a la lista de \usepackage[...]{babel}
# en docs/template/Default.latex (la plantilla debe poder cargar todos los
# idiomas que vaya a necesitar seleccionar, no solo el que se compile ahora).
LANG_SUFFIX_TO_BABEL=(
    "en:english"
    "es:spanish"
)
DEFAULT_LANG_SUFFIX="es"

if ! command -v pandoc >/dev/null 2>&1; then
    echo "build_docs_pdf: falta 'pandoc' (necesario para convertir .md -> PDF)" >&2
    exit 1
fi
if ! command -v pdflatex >/dev/null 2>&1; then
    echo "build_docs_pdf: falta 'pdflatex' (motor PDF usado por la plantilla)" >&2
    exit 1
fi
if [ ! -f "$TEMPLATE" ]; then
    echo "build_docs_pdf: no se encuentra la plantilla $TEMPLATE" >&2
    exit 1
fi
if [ ! -f "$LOGO" ]; then
    echo "build_docs_pdf: no se encuentra el logo $LOGO" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

shopt -s nullglob
mdfiles=("$RAW_DIR"/*.md)
shopt -u nullglob

if [ ${#mdfiles[@]} -eq 0 ]; then
    echo "build_docs_pdf: no hay ficheros .md en $RAW_DIR" >&2
    exit 1
fi

# Dado "hermes_devices.en" (nombre de fichero sin ".md"), extrae el sufijo
# tras el ultimo "." (aqui "en") y devuelve el nombre babel correspondiente
# segun LANG_SUFFIX_TO_BABEL; si el sufijo no esta en el vector (fichero sin
# marca de idioma, o idioma todavia no añadido), cae a DEFAULT_LANG_SUFFIX.
babel_lang_for() {
    local base="$1"
    local suffix="${base##*.}"
    local entry code name
    for entry in "${LANG_SUFFIX_TO_BABEL[@]}"; do
        code="${entry%%:*}"
        name="${entry#*:}"
        if [ "$suffix" = "$code" ]; then
            echo "$name"
            return 0
        fi
    done
    for entry in "${LANG_SUFFIX_TO_BABEL[@]}"; do
        code="${entry%%:*}"
        name="${entry#*:}"
        if [ "$DEFAULT_LANG_SUFFIX" = "$code" ]; then
            echo "$name"
            return 0
        fi
    done
}

fail=0
for src in "${mdfiles[@]}"; do
    base="$(basename "${src%.md}")"
    out="$OUT_DIR/$base.pdf"
    babel_lang="$(babel_lang_for "$base")"

    # Si el PDF ya existe y es mas reciente que su fuente y que todo lo que
    # puede cambiar su contenido (plantilla, logo), no hace falta recompilar.
    if [ -f "$out" ] && [ "$out" -nt "$src" ] && [ "$out" -nt "$TEMPLATE" ] && [ "$out" -nt "$LOGO" ]; then
        echo "== $base.md -> docs/refined/$base.pdf == (sin cambios, se omite)"
        continue
    fi

    echo "== $base.md ($babel_lang) -> docs/refined/$base.pdf =="
    if pandoc "$src" \
        --from markdown+smart \
        --template="$TEMPLATE" \
        --pdf-engine=pdflatex \
        --toc \
        -V babel-lang="$babel_lang" \
        -V logo-path="$LOGO" \
        -o "$out"; then
        echo "   OK"
    else
        echo "   FALLO compilando $base.md" >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "build_docs_pdf: algunos documentos fallaron (ver arriba)" >&2
    exit 1
fi

echo "Todos los PDF generados en $OUT_DIR"
