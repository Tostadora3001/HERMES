/* Solo para scripts/Compilation_Execution_test.sh: cuando main() termina,
 * espera a que salga el ultimo byte por la UART y duerme la CPU con las
 * interrupciones desactivadas -- simavr interpreta eso como fin de programa
 * y termina limpiamente (volcando la salida de la UART). Sin esto, un
 * programa AVR que acaba se queda en el bucle infinito de exit() y simavr
 * solo se detiene al matarlo, perdiendo la salida. No forma parte del
 * framework: los programas reales no "terminan" en un microcontrolador. */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>

__attribute__((destructor)) static void hermes_test_sim_exit(void) {
    if (UCSR0B & (1 << TXEN0)) {
        while (!(UCSR0A & (1 << UDRE0))) {}
    }
    cli();
    sleep_cpu();
}
