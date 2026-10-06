# HERMES

Entorno y motor para la programación simplificada de microcontroladores.

- Especificación y definición del proyecto: `docs/raw/Especificacion.es.md`,
  `docs/raw/Definicion.es.md`.
- Decisiones de arquitectura y alcance vigentes (qué se simplificó respecto
  a la especificación original y por qué): `CLAUDE.md`.
- Subconjunto de sintaxis HERMESPY soportado actualmente:
  `docs/raw/Subconjunto-HERMESPY.v0.1.es.md`.
- Runtime propio de hilos y semáforos: `docs/raw/hermes_threads.en.md`.
- Paralelismo real multinúcleo: `docs/raw/hermes_parallel.en.md`.
- Perfiles de memoria estáticos (light/medium/heavy): `docs/raw/hermes_mempool.en.md`.
- Devices (internos/externos) y sintaxis de método: `docs/raw/hermes_devices.en.md`.

## Estado actual

Fase 1 completa y Fase 2 en curso. HERMESPY funciona como un **transpilador
por reglas fijas** de `.hepy` a C (`hepyc`), cuyo resultado se compila con
`gcc`/`avr-gcc` según destino. Ya existe un **runtime de hilos propio** (sin
RTOS de terceros) para **host x86-64 y AVR real (ATmega328P)**: memoria por
hilo (TCB + heap + stack en un único bloque), scheduler cooperativo
round-robin, context-switch en ensamblador (uno por arquitectura), y
semáforos contadores — expuesto a HERMESPY vía `import hermes_threads`
(`thread_start`/`threads_run` en modo automático, `thread_start_ex` +
semáforos en modo concreto). `hepyc` elige la arquitectura (o varias a la
vez) según los imports `native`/`avr` del propio `.hepy` — ver
`examples/threads_multiarch.hepy`. Además, `import hermes_parallel` da
paralelismo **real** entre núcleos físicos (`pthread` en el host) para
bucles CPU-bound, como modelo separado del scheduler cooperativo — ver
`examples/parallel_squares.hepy`. Los imports `light`/`medium`/`heavy`
cambian los bloques de hilo/semáforo de `malloc` dinámico a un pool
estático de tamaño fijo (1024/4096/16384 B) — ver
`examples/threads_pool_light.hepy`. Los recursos (hilos, hardware) son
**devices** con sintaxis de método (`receptor.metodo(args)`, resuelta en
tiempo de compilación, sin despacho dinámico): el hilo ya existente es el
primer device interno (`t.join()`), y hay un pin GPIO digital de salida
como primer device externo (`led.write(True)`), con registros reales en
AVR y mock en host — ver `examples/devices_demo.hepy`. Todavía no hay GUI
ni analizador estático — ver `CLAUDE.md` para la hoja de ruta por fases.

## Construir

Requiere `gcc` y, para el build con CMake, `cmake` (>= 3.16). Nota: el build
con CMake aún no se ha podido verificar en este entorno porque `cmake` no
está instalado; mientras tanto se puede compilar `hepyc` directamente:

```sh
gcc -std=c11 -Wall -Wextra -Isrc/engine/compiler \
    src/engine/compiler/main.c src/engine/compiler/lexer.c \
    src/engine/compiler/ast.c src/engine/compiler/parser.c \
    src/engine/compiler/codegen.c src/engine/compiler/arch.c \
    src/engine/compiler/mempool.c src/engine/compiler/devicekind.c \
    -o build/hepyc
```

Con CMake, una vez instalado:

```sh
mkdir -p build && cd build
cmake ..
cmake --build .
ctest
```

## Probar un ejemplo

```sh
./build/hepyc examples/factorial.hepy -o build/factorial.c \
    --compile build/factorial -I src/useful
./build/factorial
```

## Generar la documentación en PDF

```sh
./scripts/build_docs_pdf.sh
```

Convierte cada `.md` de `docs/raw` a PDF usando la plantilla
`docs/template/Default.latex` (paleta azul cielo, con una sandalia alada de
Hermes como motivo recurrente) y deja el resultado en `docs/refined/`
(generado, no versionado — ver `.gitignore`). Requiere `pandoc` y
`pdflatex`.
