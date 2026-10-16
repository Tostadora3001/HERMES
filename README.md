# HERMES

Entorno y motor para la programación simplificada de microcontroladores.

- **Para empezar**: guía de usuario, una introducción ligera con lo más
  relevante: `docs/raw/USER_GUIDE.es.md`.
- Especificación y definición del proyecto: `docs/raw/Especificacion.es.md`,
  `docs/raw/Definicion.es.md`.
- Decisiones de arquitectura y alcance vigentes (qué se simplificó respecto
  a la especificación original y por qué): `CLAUDE.md`.
- Subconjunto de sintaxis HERMESPY soportado actualmente:
  `docs/raw/Subconjunto-HERMESPY.v0.1.es.md`.
- Índice de todos los `import` disponibles: `docs/raw/hermes_imports.es.md`.
- El transpilador `hepyc` (pipeline, reglas, diagnósticos): `docs/raw/hermes_compiler.en.md`.
- Biblioteca de hilos, semáforos y mutex (canarios, mensajes de error): `docs/raw/hermes_threads.en.md`.
- Bucles paralelos (`parallel for`): `docs/raw/hermes_parallel.en.md`.
- Mini SO (planificadores, tick, preemption, herencia de prioridad, test de factibilidad, rendimiento medido): `docs/raw/hermes_os.en.md`.
- Perfiles de hilo (light/medium/heavy, tamaño de bloque por hilo): `docs/raw/hermes_thread_profiles.en.md`.
- Devices, registro, handles y sintaxis de método: `docs/raw/hermes_devices.en.md`.
- Módulos de hardware (`modules/<nombre>/`, `module.hmod`, cómo escribir uno): `docs/raw/hermes_modules.en.md`.
- Capa de drivers (timer, GPIO, UART, ADC, PWM, SPI) y enlazado automático: `docs/raw/hermes_drivers.en.md`.
- Timers como devices con papel: `docs/raw/hermes_timers.en.md`.
- Bus I2C: `docs/raw/hermes_i2c.en.md`.
- Paquetes fiables por un puerto serie: `docs/raw/hermes_packet.en.md`.
- Sockets TCP/UDP, servidores, DNS y DHCP, y los módulos Ethernet W5500 y Wi-Fi ESP-01: `docs/raw/hermes_net.en.md`.
- Bluetooth (módulos HC-05 y HM-10): `docs/raw/hermes_bluetooth.en.md`.
- Placas, plataformas y arquitecturas (Uno, Mega, ESP32-C3) y qué está verificado: `docs/raw/hermes_boards.en.md`.
- `print()` y utilidades mínimas: `docs/raw/hermes_stdlib.en.md`.

## Estado actual

HERMESPY funciona como un **transpilador por reglas fijas** de `.hepy` a C
(`hepyc`), cuyo resultado se compila con `gcc`/`avr-gcc`/
`riscv64-unknown-elf-gcc` según destino: PC, Arduino Uno y Mega, y
ESP32-C3 (`docs/raw/hermes_boards.en.md`). Sobre eso, un framework propio
en C, sin RTOS de terceros:

- **Hilos** (`import hermes_threads`): memoria por hilo (TCB + heap + pila
  en un bloque, con canarios), cambio de contexto en ensamblador por
  arquitectura, varios núcleos (pthreads en el PC), semáforos y mutex.
  Sin planificador: el programa decide quién corre (`t.run()`,
  `t.launch()`, `t.wait()`). `parallel for` reparte un bucle entre los
  núcleos libres (`import hermes_parallel`).
- **Mini SO** (`import hermes_os` + un planificador: `os_priority`,
  `os_deadline` u `os_cyclic`): tick cada ms, preemption en bare metal,
  hilos dormidos, herencia de prioridad, migración entre núcleos, test de
  factibilidad EDF. Ver `docs/raw/hermes_os.en.md` (incluye el
  rendimiento medido en el Uno y la ESP32-C3).
- **Devices** con sintaxis de método (`led.write(True)`, resuelta al
  transpilar, sin despacho dinámico): hilos, UART, ADC, PWM, timers, I2C,
  SPI (núcleo), GPIO, Ethernet W5500, Wi-Fi ESP-01 y Bluetooth HC-05 /
  HM-10 (módulos).
- **Comunicaciones**: varios puertos serie, paquetes fiables con CRC y
  reintentos (`hermes_packet`), sockets TCP/UDP, servidores, DNS y DHCP
  (`hermes_net`) por Ethernet o Wi-Fi, Bluetooth. Detección de conflictos de pines,
  borrado, volcado, `abort` de todo ante un error fatal.
- **Errores** siempre en la línea del `.hepy` y en inglés; en AVR, los del
  framework salen por la UART (`import hermes_uart`).

Todo verificado en el PC, en simavr (Uno y Mega) y en el QEMU de
Espressif (ESP32-C3); **nada todavía en placa física**. Sin GUI, analizador
estático, simulador con breakpoints ni flasheo integrado — ver `CLAUDE.md`
para lo pendiente.

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
con `tests/expected/<ejemplo>.out`; por cada ejemplo multiarquitectura
añade una prueba por placa (`<ejemplo>_uno`, `_mega`, `_c3`): su salida
por la UART bajo simavr (Uno, Mega) o el QEMU de Espressif (C3) contra la
salida esperada. Una placa sin sus herramientas, o marcada en el ejemplo
con `# skip <placa>: motivo`, se salta. Es la misma comprobación que
`scripts/Compilation_Execution_test.sh`, prueba a prueba (lo común está en
`scripts/board_lib.sh`; ver "Verification" en `docs/raw/hermes_boards.en.md`).

## Probar todo

```sh
./scripts/Compilation_Execution_test.sh
```

Compila `hepyc` (y el banco de pruebas `scripts/simavr_bench`, si está
`libsimavr-dev`),
compila y ejecuta todos los `examples/*.hepy`, compara la salida de cada
uno con `tests/expected/<ejemplo>.out` y, en los multiarquitectura, la de
cada placa: Uno y Mega bajo `simavr`, ESP32-C3 bajo el QEMU de Espressif
(`qemu-system-riscv32-esp`). Un fallo no detiene el resto: se resumen al
final. Requiere `gcc`; para las placas, `avr-gcc` + `simavr` y
`riscv64-unknown-elf-gcc` + el QEMU de Espressif (sin ellos, esas
variantes se saltan).

Solo los ejemplos de una librería:

```sh
./scripts/test_hermes_stdlib.sh     # hello, factorial, fizzbuzz, insert_c
./scripts/test_hermes_threads.sh    # threads_*
./scripts/test_hermes_parallel.sh   # parallel_squares, parallel_sum
./scripts/test_hermes_devices.sh    # devices_*, abort_demo
./scripts/test_hermes_drivers.sh    # drivers_*, timers_roles, i2c_*
./scripts/test_hermes_os.sh         # os_*
./scripts/test_hermes_comm.sh       # comm_* (puertos serie, paquetes, red)
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
