# HERMES

Entorno y motor para la programación simplificada de microcontroladores.

- Especificación y definición del proyecto: `docs/raw/Especificacion.es.md`,
  `docs/raw/Definicion.es.md`.
- Decisiones de arquitectura y alcance vigentes (qué se simplificó respecto
  a la especificación original y por qué): `CLAUDE.md`.
- Subconjunto de sintaxis HERMESPY soportado actualmente:
  `docs/raw/Subconjunto-HERMESPY.v0.1.es.md`.
- Biblioteca de hilos y semáforos (canarios, mensajes de error): `docs/raw/hermes_threads.en.md`.
- Bucles paralelos (`parallel for`): `docs/raw/hermes_parallel.en.md`.
- Mini SO (planificador por prioridades, migración entre núcleos, proceso): `docs/raw/hermes_os.en.md`.
- Perfiles de hilo (light/medium/heavy, tamaño de bloque por hilo): `docs/raw/hermes_thread_profiles.en.md`.
- Devices (internos/externos), registro de devices y sintaxis de método: `docs/raw/hermes_devices.en.md`.
- Módulos de hardware (`modules/<nombre>/`, `module.hmod`): `docs/raw/hermes_modules.en.md`.
- Capa de drivers (timer, GPIO, UART) y enlazado automático: `docs/raw/hermes_drivers.en.md`.
- `print()` y utilidades mínimas: `docs/raw/hermes_stdlib.en.md`.
- Índice de todos los `import` disponibles: `docs/raw/hermes_imports.es.md`.

## Estado actual

Fase 1 completa y Fase 2 en curso. HERMESPY funciona como un **transpilador
por reglas fijas** de `.hepy` a C (`hepyc`), cuyo resultado se compila con
`gcc`/`avr-gcc`/`riscv64-unknown-elf-gcc` según destino (placas: Uno, Mega
y ESP32-C3, ver `docs/raw/hermes_boards.en.md`). Ya existe un **runtime de
hilos propio** (sin RTOS de terceros) para **host x86-64, AVR (ATmega328P,
ATmega2560) y RISC-V (ESP32-C3)**: memoria por
hilo (TCB + heap + stack en un único bloque), context-switch en
ensamblador (uno por arquitectura), varios núcleos (pthreads en el host) y
semáforos contadores — **sin planificador**: el programa decide quién corre
(`t.run()` en su núcleo, `t.launch()` en otro, `t.wait()`). Expuesto a
HERMESPY vía `import hermes_threads`. El planificador lo aporta, aparte,
el **mini SO** (`import hermes_os` + `import os_priority`): prioridades,
migración de hilos entre núcleos y un proceso que agrupa los bloques de sus
hilos — ver `examples/os_priority_demo.hepy`. Cada bloque de hilo lleva
canarios para detectar corrupciones de memoria, y en AVR los errores del
framework salen por la UART si se importa `hermes_uart`. `hepyc` elige la arquitectura (o varias a la
vez) según los imports `native`/`avr` del propio `.hepy` — ver
`examples/threads_multiarch.hepy`. Además, `import hermes_parallel` añade
la instrucción `parallel for` (con variables `local`, `global` y
reducciones `global(+)`...), que reparte un bucle entre los núcleos libres
sobre esos mismos hilos — ver `examples/parallel_sum.hepy`. Los imports `light`/`medium`/`heavy`
eligen el tamaño por defecto del bloque (heap + pila) de cada hilo — ver
`examples/threads_light.hepy`. Los recursos (hilos, hardware) son
**devices** con sintaxis de método (`receptor.metodo(args)`, resuelta en
tiempo de compilación, sin despacho dinámico): el hilo ya existente es el
primer device interno (`t.run()`, `t.wait()`...), y hay un pin GPIO digital de salida
como primer device externo (`led.write(True)`), con registros reales en
AVR y mock en host — ver `examples/devices_demo.hepy`. Todavía no hay GUI
ni analizador estático — ver `CLAUDE.md` para la hoja de ruta por fases.

## Construir

Requiere `gcc` y, para el build con CMake, `cmake` (>= 3.16; verificado con
3.28). Para cada destino, su toolchain: `avr-gcc` (+ `simavr` para las
pruebas) para AVR; para la ESP32-C3, `gcc-riscv64-unknown-elf` +
`picolibc-riscv64-unknown-elf` (apt) y, para emularla, el QEMU de
Espressif (`qemu-system-riscv32 -M esp32c3`, binario de
github.com/espressif/qemu/releases; el QEMU de Ubuntu no trae esa
máquina). También se puede compilar `hepyc` directamente:

```sh
gcc -std=c11 -Wall -Wextra -Isrc/engine/compiler \
    src/engine/compiler/main.c src/engine/compiler/lexer.c \
    src/engine/compiler/ast.c src/engine/compiler/parser.c \
    src/engine/compiler/codegen.c src/engine/compiler/arch.c src/engine/compiler/board.c \
    src/engine/compiler/threadprofile.c src/engine/compiler/devicekind.c \
    src/engine/compiler/module.c src/engine/compiler/corelib.c \
    src/engine/compiler/repo.c src/engine/compiler/diagnostics.c \
    -o build/hepyc
```

Con CMake:

```sh
cmake -S . -B build-cmake
cmake --build build-cmake
cd build-cmake && ctest --output-on-failure
```

`ctest` ejecuta una prueba por ejemplo de `examples/` y compara su salida
con `tests/expected/<ejemplo>.out`; si encuentra `simavr` y `avr-gcc`,
añade una prueba AVR por cada ejemplo multiarquitectura (su salida por la
UART bajo simavr contra la misma salida esperada). Es la misma
comprobación que `scripts/Compilation_Execution_test.sh`, prueba a prueba.

## Probar todo

```sh
./scripts/Compilation_Execution_test.sh
```

Compila `hepyc`, compila y ejecuta todos los `examples/*.hepy`, compara
la salida de cada uno con `tests/expected/<ejemplo>.out` y, en los que se
generan también para AVR, su salida bajo `simavr` (por la UART) con la
nativa. Un fallo no detiene el resto: se resumen al final. Requiere `gcc`,
`avr-gcc` y `simavr`.

Solo los ejemplos de una librería:

```sh
./scripts/test_hermes_stdlib.sh     # hello, factorial, fizzbuzz, insert_c
./scripts/test_hermes_threads.sh    # threads_*
./scripts/test_hermes_parallel.sh   # parallel_squares, parallel_sum
./scripts/test_hermes_devices.sh    # devices_demo, devices_registry, abort_demo
./scripts/test_hermes_drivers.sh    # drivers_demo
./scripts/test_hermes_os.sh         # os_priority_demo, os_cores
```

Qué ejemplos van con cada librería se define en un único sitio,
`scripts/examples_lib.sh` (la lógica común de todos estos scripts); el
script completo avisa de cualquier ejemplo que no esté en ninguna lista.

## Probar un ejemplo

```sh
./build/hepyc examples/factorial.hepy -o build/factorial.c \
    --compile build/factorial
./build/factorial
```

`hepyc` añade por sí mismo los `-I` del framework y enlaza lo que pide cada
import (runtime de hilos, drivers, módulos): no hace falta `--src` ni `-I`
para nada que venga con HERMES.

El `.c` generado lleva directivas `#line` (para que los errores salgan en
la línea del `.hepy`). Para leerlo sin ellas:

```sh
./scripts/clean_c.sh build/factorial.c            # copia en build/clean_build/
./scripts/clean_c.sh -o otra/carpeta build/*.c    # -o / --out-dir: otra ruta
```

## Generar la documentación en PDF

```sh
./scripts/build_docs_pdf.sh
```

Convierte cada `.md` de `docs/raw` a PDF usando la plantilla
`docs/template/Default.latex` (paleta azul cielo, con el logo del proyecto
—un caduceo— en la portada y en la cabecera de cada página) y deja el resultado en `docs/refined/`
(generado, no versionado — ver `.gitignore`). Requiere `pandoc` y
`pdflatex`.
