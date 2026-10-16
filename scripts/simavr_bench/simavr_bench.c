/* simavr_bench: simavr plus the hardware HERMES's AVR tests need around
 * the chip -- I2C chips, wires between UARTs, and (later) SPI chips and
 * serial modules. Test only: not part of the framework (needs
 * libsimavr-dev). Each example says what it needs in a comment line
 * `# simavr_bench: <args>`, read by the suite and ctest
 * (scripts/board_lib.sh).
 *
 *   simavr_bench [-m mcu] [-f hz] [options]... firmware.elf
 *
 * I2C:
 *   --eeprom addr   a 256-byte EEPROM (24C02-like, simavr's i2c_eeprom part,
 *                   zeroed) answering at the 7-bit address `addr`
 *   --master script the bench is a master talking to the AVR as slave;
 *                   steps separated by ';', run in order, each when the
 *                   previous one ended:
 *                     @ms       wait until that simulated time
 *                     w A b...  write bytes b... to address A
 *                     r A n     read n bytes from address A
 *                   what it sees is printed as "[i2c master] ..." lines
 *                   (on stderr, like simavr's UART lines, so both keep
 *                   their order)
 * UART:
 *   --wire A:B      USART A's TX into USART B's RX and B's TX into A's RX
 *                   (A == B: a loopback). Those ports stop printing on
 *                   the console; USART0 keeps printing.
 *   --drop N        on the wires, every Nth byte is lost
 *   --flip N        on the wires, every Nth byte arrives with a bit
 *                   flipped (counted apart from --drop)
 *   Both report on stderr ("[wire] dropped/flipped byte k").
 * Serial modules:
 *   --esp01 N       an ESP-01 Wi-Fi module (AT firmware) on USART N. A
 *                   virtual one (modules/esp01/esp01_virtual.c, the esp01
 *                   module's mock on the host): its connections are the
 *                   PC's own sockets.
 *   --hc05 N        an HC-05 Bluetooth module on USART N, paired to a
 *                   device that echoes (modules/hc05/bt_virtual.c)
 *   --hm10 N        an HM-10 BLE module on USART N: AT set-up, then a
 *                   central connects and echoes (the same file)
 *   Their bytes go in at most one per byte time at 115200 baud, and never
 *   into a full receive FIFO.
 * SPI:
 *   --w5500 PB      a W5500 Ethernet chip on the SPI bus, its chip select
 *                   on port P bit B (e.g. B4: the Mega's D10; B2: the
 *                   Uno's D10). A virtual one (modules/w5500/w5500_virtual.c,
 *                   the same as the w5500 module's mock on the host): its
 *                   sockets are the PC's own -- a TCP CONNECT to
 *                   127.0.0.1:7 really connects there.
 *
 * simavr 1.6's TWI as a slave (sim/avr_twi.c) needs care, so the master
 *   here works around it:
 *   - any message with TWI_COND_WRITE that reaches a selected slave is
 *     taken as a received byte (its data field), the START too: so a
 *     write sends its first byte inside the START (simavr then skips the
 *     "own SLA+W" status and hands the byte over, 0x80);
 *   - a STOP sets a status: with TWI_COND_WRITE, a received byte (0);
 *     without, "own SLA+R" (0xA8), as if a read started. So no STOP is
 *     sent: the next START resets simavr's state anyway, and the driver's
 *     slave needs no STOP;
 *   - as a transmitter it only sends the byte loaded on SLA+R (requests
 *     for more aren't handled): a read of n bytes is n transfers of one;
 *   - one pending status at a time (9 us later): messages go 200 us apart;
 *   - a START compares the 7-bit address as it is (not shifted, no R/W
 *     bit) with TWAR >> 1; the R/W comes from TWI_COND_WRITE.
 * Its master side has quirks of its own, worked around in the driver
 * (hermes_drv_i2c_avr.c: tw_result and the codes after SLA+W).
 * The UART comes out as with simavr (lines on stdout). It stops when the
 * firmware stops (scripts/avr_sim_exit.c: sleep with interrupts off) or
 * after 20 s of simulated time. */
#include <stddef.h> /* (i2c_eeprom.h uses size_t without it) */
#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>
#include <simavr/avr_twi.h>
#include <simavr/avr_uart.h>
#include <simavr/avr_spi.h>
#include <simavr/avr_ioport.h>
#include "../../modules/w5500/w5500_virtual.h"
#include "../../modules/esp01/esp01_virtual.h"
#include "../../modules/hc05/bt_virtual.h"
#include <simavr/parts/i2c_eeprom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EEPROMS 4

static avr_t *g_avr;

/* --- SPI: a virtual W5500 ---------------------------------------------------- */
static w5500v_t *g_w5500;
static avr_irq_t *g_spi_in;
static int g_w5500_selected;

static void w5500_cs(struct avr_irq_t *irq, uint32_t v, void *param) {
    (void)irq; (void)param;
    int sel = v == 0;
    if (sel == g_w5500_selected) return;
    g_w5500_selected = sel;
    w5500v_select(g_w5500, sel);
}

/* a byte the AVR clocked out: the chip's answer is what it clocks in */
static void w5500_spi(struct avr_irq_t *irq, uint32_t v, void *param) {
    (void)irq; (void)param;
    uint8_t out = g_w5500_selected ? w5500v_transfer(g_w5500, (uint8_t)v) : 0xFF;
    avr_raise_irq(g_spi_in, out);
}

/* --- UART wires ---------------------------------------------------------- */
#define MAX_WIRES 4
static int g_drop_every, g_flip_every;
static unsigned long g_wire_bytes;

/* a byte left USART `from` (param = the RX irq at the other end) */
static void wire_out(struct avr_irq_t *irq, uint32_t v, void *param) {
    (void)irq;
    unsigned long k = ++g_wire_bytes;
    if (g_drop_every && k % (unsigned long)g_drop_every == 0) {
        fprintf(stderr, "[wire] dropped byte %lu\n", k);
        return;
    }
    if (g_flip_every && k % (unsigned long)g_flip_every == 0) {
        fprintf(stderr, "[wire] flipped byte %lu\n", k);
        v ^= 0x10;
    }
    avr_raise_irq((avr_irq_t *)param, v & 0xFF);
}

/* USART `n`: no console printing, TX to `to`'s RX */
static void wire_one(int n, int to) {
    uint32_t flags = 0;
    avr_ioctl(g_avr, AVR_IOCTL_UART_GET_FLAGS('0' + n), &flags);
    flags &= ~AVR_UART_FLAG_STDIO;
    avr_ioctl(g_avr, AVR_IOCTL_UART_SET_FLAGS('0' + n), &flags);
    avr_irq_t *tx = avr_io_getirq(g_avr, AVR_IOCTL_UART_GETIRQ('0' + n), UART_IRQ_OUTPUT);
    avr_irq_t *rx = avr_io_getirq(g_avr, AVR_IOCTL_UART_GETIRQ('0' + to), UART_IRQ_INPUT);
    if (!tx || !rx) { fprintf(stderr, "[wire] no USART%d or USART%d on this chip\n", n, to); exit(2); }
    avr_irq_register_notify(tx, wire_out, rx);
}
static avr_irq_t *g_in;
static int g_acked, g_got, g_data; /* what the slave answered (output irq) */

static void slave_out(struct avr_irq_t *irq, uint32_t v, void *param) {
    (void)irq; (void)param;
    avr_twi_msg_irq_t m;
    m.u.v = v;
    if ((m.u.twi.msg & TWI_COND_ADDR) && (m.u.twi.msg & TWI_COND_ACK)) g_acked = 1;
    if ((m.u.twi.msg & TWI_COND_ADDR) && (m.u.twi.msg & TWI_COND_READ)) { g_got = 1; g_data = m.u.twi.data; }
}

/* Runs the AVR until cycle `end`; 0 if the firmware stopped. */
/* --- virtual serial modules on the USARTs ----------------------------------------- */
#define SERIAL_PORTS 4
static struct {
    void (*input)(void *ctx, uint8_t b);
    int (*output)(void *ctx);
    void *ctx;
    avr_irq_t *in;
    int xoff;
    avr_cycle_count_t next;
} g_ser[SERIAL_PORTS];
static int g_nser;

static void ser_from_avr(struct avr_irq_t *irq, uint32_t v, void *param) {
    (void)irq;
    int n = (int)(intptr_t)param;
    g_ser[n].input(g_ser[n].ctx, (uint8_t)v);
}
static void ser_xon(struct avr_irq_t *irq, uint32_t v, void *param) { (void)irq; (void)v; g_ser[(intptr_t)param].xoff = 0; }
static void ser_xoff(struct avr_irq_t *irq, uint32_t v, void *param) { (void)irq; (void)v; g_ser[(intptr_t)param].xoff = 1; }

/* one byte per byte time at 115200 baud, when the FIFO has room */
static void serial_step(void) {
    for (int n = 0; g_nser && n < SERIAL_PORTS; n++) {
        if (!g_ser[n].output || g_avr->cycle < g_ser[n].next || g_ser[n].xoff) continue;
        g_ser[n].next = g_avr->cycle + g_avr->frequency / 11520;
        int b = g_ser[n].output(g_ser[n].ctx);
        if (b >= 0) avr_raise_irq(g_ser[n].in, (uint32_t)b);
    }
}

static void esp01_in(void *c, uint8_t b) { esp01v_input(c, b); }
static int esp01_out(void *c) { return esp01v_output(c); }
static void bt_in(void *c, uint8_t b) { btv_input(c, b); }
static int bt_out(void *c) { return btv_output(c); }

static void serial_attach(const char *what, int n, void (*input)(void *, uint8_t), int (*output)(void *), void *ctx) {
    char u = (char)('0' + n);
    avr_irq_t *in = n >= 0 && n < SERIAL_PORTS ? avr_io_getirq(g_avr, AVR_IOCTL_UART_GETIRQ(u), UART_IRQ_INPUT) : NULL;
    avr_irq_t *tx = in ? avr_io_getirq(g_avr, AVR_IOCTL_UART_GETIRQ(u), UART_IRQ_OUTPUT) : NULL;
    if (!in || !tx) { fprintf(stderr, "[%s] no USART%d on this chip\n", what, n); exit(2); }
    g_ser[n].input = input;
    g_ser[n].output = output;
    g_ser[n].ctx = ctx;
    g_ser[n].in = in;
    g_nser++;
    avr_irq_register_notify(tx, ser_from_avr, (void *)(intptr_t)n);
    avr_irq_register_notify(avr_io_getirq(g_avr, AVR_IOCTL_UART_GETIRQ(u), UART_IRQ_OUT_XON), ser_xon, (void *)(intptr_t)n);
    avr_irq_register_notify(avr_io_getirq(g_avr, AVR_IOCTL_UART_GETIRQ(u), UART_IRQ_OUT_XOFF), ser_xoff, (void *)(intptr_t)n);
    uint32_t flags = 0; /* its traffic isn't the program's output */
    avr_ioctl(g_avr, AVR_IOCTL_UART_GET_FLAGS(u), &flags);
    flags &= ~AVR_UART_FLAG_STDIO;
    avr_ioctl(g_avr, AVR_IOCTL_UART_SET_FLAGS(u), &flags);
}

static int run_until(avr_cycle_count_t end) {
    while (g_avr->cycle < end) {
        int state = avr_run(g_avr);
        serial_step();
        /* simavr itself turns "asleep with interrupts off" (avr_sim_exit.c)
         * into cpu_Done -- at the right moment: checking sreg here instead
         * caught the OS's idle `sei; sleep` (the I flag is set one
         * instruction late) as an end */
        if (state == cpu_Done || state == cpu_Crashed) return 0;
    }
    return 1;
}

static int msg(uint8_t m, uint8_t addr, uint8_t data) {
    avr_raise_irq(g_in, avr_twi_irq_msg(m, addr, data));
    return run_until(g_avr->cycle + g_avr->frequency / 5000); /* 200 us */
}

/* One step of the master's script; 0 if the firmware stopped. */
static int step(char *st) {
    char *tok = strtok(st, " \t");
    if (!tok) return 1;
    if (tok[0] == '@') {
        return run_until((avr_cycle_count_t)(strtod(tok + 1, NULL) * g_avr->frequency / 1000));
    }
    char *a = strtok(NULL, " \t");
    if (!a) return 1;
    int addr = (int)strtol(a, NULL, 0);
    if (!strcmp(tok, "w")) {
        char *b = strtok(NULL, " \t");
        if (!b) return 1; /* (a write without bytes can't be expressed) */
        g_acked = 0;
        if (!msg(TWI_COND_START | TWI_COND_ADDR | TWI_COND_WRITE, (uint8_t)addr, (uint8_t)strtol(b, NULL, 0)))
            return 0;
        int ok = g_acked, n = 1;
        for (; ok && (b = strtok(NULL, " \t")); n++)
            if (!msg(TWI_COND_WRITE, (uint8_t)addr, (uint8_t)strtol(b, NULL, 0))) return 0;
        if (ok) fprintf(stderr, "[i2c master] wrote %d bytes to 0x%02x\n", n, addr);
        else fprintf(stderr, "[i2c master] write to 0x%02x: nack\n", addr);
    } else if (!strcmp(tok, "r")) {
        char *c = strtok(NULL, " \t");
        int n = c ? atoi(c) : 1;
        fprintf(stderr, "[i2c master] read from 0x%02x:", addr);
        for (int i = 0; i < n; i++) {
            g_got = 0;
            if (!msg(TWI_COND_START | TWI_COND_ADDR, (uint8_t)addr, 0)) return 0;
            if (g_got) fprintf(stderr, " %d", g_data);
            else fprintf(stderr, " nack");
        }
        fprintf(stderr, "\n");
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *mcu = "atmega328p", *fw = NULL, *script = NULL;
    unsigned long freq = 16000000;
    int eeprom_addr[MAX_EEPROMS], neeproms = 0;
    int wire_a[MAX_WIRES], wire_b[MAX_WIRES], nwires = 0;
    const char *w5500_cs_pin = NULL;
    int esp01_usart = -1, hc05_usart = -1, hm10_usart = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) mcu = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) freq = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--eeprom") && i + 1 < argc && neeproms < MAX_EEPROMS)
            eeprom_addr[neeproms++] = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--master") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--wire") && i + 1 < argc && nwires < MAX_WIRES) {
            if (sscanf(argv[++i], "%d:%d", &wire_a[nwires], &wire_b[nwires]) == 2) nwires++;
        }
        else if (!strcmp(argv[i], "--drop") && i + 1 < argc) g_drop_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flip") && i + 1 < argc) g_flip_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--w5500") && i + 1 < argc) w5500_cs_pin = argv[++i];
        else if (!strcmp(argv[i], "--esp01") && i + 1 < argc) esp01_usart = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hc05") && i + 1 < argc) hc05_usart = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hm10") && i + 1 < argc) hm10_usart = atoi(argv[++i]);
        else fw = argv[i];
    }
    if (!fw) {
        fprintf(stderr, "usage: %s [-m mcu] [-f hz] [--eeprom addr]... [--master script] "
                        "[--wire A:B]... [--drop N] [--flip N] [--w5500 PB] [--esp01 N] [--hc05 N] [--hm10 N] firmware.elf\n", argv[0]);
        return 2;
    }
    elf_firmware_t f;
    memset(&f, 0, sizeof(f));
    if (elf_read_firmware(fw, &f)) { fprintf(stderr, "can't read %s\n", fw); return 2; }
    g_avr = avr_make_mcu_by_name(mcu);
    if (!g_avr) { fprintf(stderr, "unknown mcu %s\n", mcu); return 2; }
    avr_init(g_avr);
    avr_load_firmware(g_avr, &f);
    g_avr->frequency = freq;

    static i2c_eeprom_t ee[MAX_EEPROMS];
    for (int i = 0; i < neeproms; i++) {
        uint8_t zero[256] = { 0 };
        i2c_eeprom_init(g_avr, &ee[i], (uint8_t)(eeprom_addr[i] << 1), 0x01, zero, sizeof(zero));
        i2c_eeprom_attach(g_avr, &ee[i], AVR_IOCTL_TWI_GETIRQ(0));
    }
    for (int i = 0; i < nwires; i++) {
        wire_one(wire_a[i], wire_b[i]);
        if (wire_b[i] != wire_a[i]) wire_one(wire_b[i], wire_a[i]);
    }

    if (esp01_usart >= 0) serial_attach("esp01", esp01_usart, esp01_in, esp01_out, esp01v_new());
    if (hc05_usart >= 0) serial_attach("hc05", hc05_usart, bt_in, bt_out, btv_new_hc05());
    if (hm10_usart >= 0) serial_attach("hm10", hm10_usart, bt_in, bt_out, btv_new_hm10());

    if (w5500_cs_pin) {
        g_w5500 = w5500v_new();
        g_spi_in = avr_io_getirq(g_avr, AVR_IOCTL_SPI_GETIRQ(0), SPI_IRQ_INPUT);
        avr_irq_register_notify(avr_io_getirq(g_avr, AVR_IOCTL_SPI_GETIRQ(0), SPI_IRQ_OUTPUT), w5500_spi, NULL);
        avr_irq_t *cs = avr_io_getirq(g_avr, AVR_IOCTL_IOPORT_GETIRQ(w5500_cs_pin[0]), w5500_cs_pin[1] - '0');
        if (!g_spi_in || !cs) { fprintf(stderr, "[w5500] no SPI or no pin %s on this chip\n", w5500_cs_pin); return 2; }
        avr_irq_register_notify(cs, w5500_cs, NULL);
    }

    g_in = avr_io_getirq(g_avr, AVR_IOCTL_TWI_GETIRQ(0), TWI_IRQ_INPUT);
    avr_irq_register_notify(avr_io_getirq(g_avr, AVR_IOCTL_TWI_GETIRQ(0), TWI_IRQ_OUTPUT), slave_out, NULL);

    int alive = 1;
    if (script) {
        char *copy = strdup(script), *save = NULL;
        for (char *st = strtok_r(copy, ";", &save); st && alive; st = strtok_r(NULL, ";", &save))
            alive = step(st);
        free(copy);
    }
    if (alive) run_until((avr_cycle_count_t)freq * 20);
    fflush(stdout);
    return 0;
}
