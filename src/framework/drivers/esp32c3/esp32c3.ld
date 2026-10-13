/* ESP32-C3 linker script for HERMES programs (no ESP-IDF, no second-stage
 * bootloader). See docs/raw/hermes_boards.en.md.
 *
 * Direct boot: the ROM finds the magic words at flash offset 0
 * (hermes_start_esp32c3.S), maps the flash at the same offset on the
 * instruction bus (IROM, 0x42000000) and on the data bus (DROM,
 * 0x3C000000), and jumps to 0x42000008.
 *   .text            runs from flash through IROM
 *   .data            constants + initialized data (+ picolibc's TLS
 *                    template): stored in flash, copied to RAM at startup
 *                    through DROM (the instruction bus can't be read as data)
 *   .tbss, .bss      RAM, zeroed at startup (.tbss: picolibc's TLS)
 *   heap             from the end of .bss up to main's stack canary
 *                    (picolibc's malloc uses __heap_start/__heap_end)
 *   main's stack     the top HERMES_MAIN_STACK of RAM
 *
 * RAM: the SRAM's data-bus view from 0x3FC80000. The top of SRAM is kept
 * for the ROM's own data (it stays in use: e.g. its UART functions), so
 * HERMES uses the first 320 KB. */

HERMES_MAIN_STACK = 8K;

MEMORY {
    IROM (rx) : ORIGIN = 0x42000000, LENGTH = 4M
    DRAM (rw) : ORIGIN = 0x3FC80000, LENGTH = 320K
}

ENTRY(_start)

SECTIONS {
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } > IROM

    .data : ALIGN(16) {
        __data_start = .;
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(4);
        __preinit_array_start = .;
        KEEP(*(.preinit_array))
        __preinit_array_end = .;
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
        __fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(4);
        __data_end = .;
    } > DRAM AT > IROM

    /* picolibc's thread-local data (errno...): one TLS block, tp points at
     * it (one core: every hermes thread shares it). .tdata is its
     * template, copied with .data; .tbss takes no room by itself in the
     * linker's layout, so .tbss_space reserves it. */
    .tdata : ALIGN(16) {
        __tls_base = .;
        *(.tdata .tdata.*)
        . = ALIGN(4);
        __tdata_end = .;
    } > DRAM AT > IROM
    .tbss : ALIGN(4) {
        __tbss_start = .;
        *(.tbss .tbss.*)
        . = ALIGN(4);
        __tbss_end = .;
    } > DRAM
    .tbss_space (NOLOAD) : {
        . = ADDR(.tbss) + SIZEOF(.tbss);
    } > DRAM
    __data_lma = LOADADDR(.data);       /* IROM addresses; read through DROM */
    __tdata_lma = LOADADDR(.tdata);

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DRAM

    __global_pointer$ = __data_start + 0x800;

    __stack_top = ORIGIN(DRAM) + LENGTH(DRAM);
    __hermes_main_stack_bytes = HERMES_MAIN_STACK;   /* absolute: hermes_thread.c */
    __hermes_main_canary = __stack_top - HERMES_MAIN_STACK - 4;
    __heap_start = ALIGN(__bss_end, 16);
    __heap_end = __hermes_main_canary;

    ASSERT(__heap_start < __heap_end, "esp32c3.ld: no room left for the heap")

    /DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
