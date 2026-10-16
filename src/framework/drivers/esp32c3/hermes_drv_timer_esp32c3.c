/* Timer driver, ESP32-C3 implementation: SYSTIMER unit 0, a 52-bit
 * counter at 16 MHz that runs from reset. No timer of its own to
 * configure: millis/micros are the counter divided, from the first use (as
 * on AVR). The tick (hermes_timer_set_tick) uses alarm 0 and is the only
 * interrupt HERMES enables on this chip. See hermes_drv_timer.h. */
#include "hermes_drv_timer.h"
#include "hermes_esp32c3_regs.h"
#include <unistd.h> /* _exit */

static uint64_t g_base;
static uint8_t g_started = 0;

static uint64_t systimer_now(void) {
    HERMES_REG(SYSTIMER_UNIT0_OP_REG) = SYSTIMER_TIMER_UNIT0_UPDATE; /* latch */
    while (!(HERMES_REG(SYSTIMER_UNIT0_OP_REG) & SYSTIMER_UNIT0_VALUE_VALID)) {}
    uint32_t hi = HERMES_REG(SYSTIMER_UNIT0_VALUE_HI_REG);
    uint32_t lo = HERMES_REG(SYSTIMER_UNIT0_VALUE_LO_REG);
    return ((uint64_t)hi << 32) | lo;
}

void hermes_timer_init(void) {
    if (g_started) return;
    g_started = 1;
    g_base = systimer_now();
}

int hermes_timer_started(void) { return g_started; }

static uint64_t elapsed_us(void) {
    hermes_timer_init();
    return (systimer_now() - g_base) / SYSTIMER_TICKS_PER_US;
}

uint32_t hermes_timer_millis(void) {
    return (uint32_t)(elapsed_us() / 1000u);
}

uint32_t hermes_timer_micros(void) {
    return (uint32_t)elapsed_us();
}

void hermes_timer_idle(void) {
}

/* --- the tick -------------------------------------------------------------
 * SYSTIMER alarm 0, periodic every 1 ms, -> interrupt matrix -> CPU
 * interrupt HERMES_ESP32C3_TICK_INT (level, priority 1). The trap vector is
 * HERMES's own (installed on the first set_tick): 32 entries, all to one
 * handler, which serves the tick and treats anything else -- an exception,
 * or an interrupt nobody enabled -- as a crash. Before set_tick, mtvec is
 * still the ROM's. */
#define HERMES_ESP32C3_TICK_INT 1
#define HERMES_ESP32C3_TICK_PERIOD (1000u * SYSTIMER_TICKS_PER_US) /* 1 ms */

static void (*volatile g_tick)(void);

/* Vectored mode: the CPU jumps to base + 4 * cause; the base must be
 * 256-byte aligned. Every entry goes to the same handler. */
__asm__(
    "    .section .text.hermes_trap_vector, \"ax\"\n"
    "    .balign 256\n"
    "    .globl hermes_esp32c3_trap_vector\n"
    "hermes_esp32c3_trap_vector:\n"
    "    .rept 32\n"
    "    j hermes_esp32c3_trap\n"
    "    .endr\n"
    "    .text\n");
extern char hermes_esp32c3_trap_vector[];

/* gcc saves what it uses (all the caller-saved registers, since it calls
 * the tick) and returns with mret. Runs on the interrupted stack.
 * The tick may switch threads (hermes_os preemption): the interrupted
 * thread then stays inside tick() until it's resumed, while other threads
 * run and take interrupts of their own -- which overwrite mepc and
 * mstatus. So both are kept here, on this thread's stack, and put back
 * before mret (interrupts are masked again by then). */
__attribute__((interrupt("machine"), used))
void hermes_esp32c3_trap(void) {
    uint32_t cause;
    __asm__ __volatile__("csrr %0, mcause" : "=r"(cause));
    if ((cause & 0x80000000u) && (cause & 0x1Fu) == HERMES_ESP32C3_TICK_INT) {
        HERMES_REG(SYSTIMER_INT_CLR_REG) = SYSTIMER_TARGET0_INT; /* level: lower it */
        void (*tick)(void) = g_tick;
        if (tick) {
            uint32_t epc, status;
            __asm__ __volatile__("csrr %0, mepc" : "=r"(epc));
            __asm__ __volatile__("csrr %0, mstatus" : "=r"(status));
            tick();
            __asm__ __volatile__("csrw mstatus, %0" :: "r"(status) : "memory");
            __asm__ __volatile__("csrw mepc, %0" :: "r"(epc) : "memory");
        }
        return;
    }
    /* an exception (illegal instruction, bad address...) or a stray
     * interrupt: returning would repeat it forever */
    _exit(128 + (int)(cause & 0x1Fu));
}

static void tick_start(void) {
    static uint8_t vector_installed = 0;
    if (!vector_installed) {
        vector_installed = 1;
        __asm__ __volatile__("csrw mtvec, %0" :: "r"((uint32_t)hermes_esp32c3_trap_vector | 1u));
    }
    /* alarm 0: unit 0, periodic, 1 ms; the mode bit is set after loading
     * the period (TRM: "Periodic alarms") */
    HERMES_REG(SYSTIMER_CONF_REG) &= ~SYSTIMER_TARGET0_WORK_EN;
    HERMES_REG(SYSTIMER_TARGET0_CONF_REG) = HERMES_ESP32C3_TICK_PERIOD & SYSTIMER_TARGET0_PERIOD_MASK;
    HERMES_REG(SYSTIMER_COMP0_LOAD_REG) = 1;
    HERMES_REG(SYSTIMER_TARGET0_CONF_REG) |= SYSTIMER_TARGET0_PERIOD_MODE;
    HERMES_REG(SYSTIMER_COMP0_LOAD_REG) = 1;
    HERMES_REG(SYSTIMER_INT_CLR_REG) = SYSTIMER_TARGET0_INT;
    HERMES_REG(SYSTIMER_INT_ENA_REG) |= SYSTIMER_TARGET0_INT;
    HERMES_REG(SYSTIMER_CONF_REG) |= SYSTIMER_TARGET0_WORK_EN;
    /* interrupt matrix: source -> CPU interrupt, level, priority 1 */
    HERMES_REG(INTMTX_MAP_REG(INTMTX_SRC_SYSTIMER_TARGET0)) = HERMES_ESP32C3_TICK_INT;
    HERMES_REG(INTMTX_CPU_INT_TYPE_REG) &= ~(1u << HERMES_ESP32C3_TICK_INT);
    HERMES_REG(INTMTX_CPU_INT_PRI_REG(HERMES_ESP32C3_TICK_INT)) = 1;
    HERMES_REG(INTMTX_CPU_INT_THRESH_REG) = 1;
    HERMES_REG(INTMTX_CPU_INT_ENABLE_REG) |= 1u << HERMES_ESP32C3_TICK_INT;
    __asm__ __volatile__("csrs mstatus, 8" ::: "memory"); /* MIE */
}

static void tick_stop(void) {
    HERMES_REG(INTMTX_CPU_INT_ENABLE_REG) &= ~(1u << HERMES_ESP32C3_TICK_INT);
    HERMES_REG(SYSTIMER_INT_ENA_REG) &= ~SYSTIMER_TARGET0_INT;
    HERMES_REG(SYSTIMER_CONF_REG) &= ~SYSTIMER_TARGET0_WORK_EN;
    HERMES_REG(SYSTIMER_INT_CLR_REG) = SYSTIMER_TARGET0_INT;
}

void hermes_timer_set_tick(void (*fn)(void)) {
    hermes_timer_init();
    void (*before)(void) = g_tick;
    g_tick = fn; /* one word: never torn */
    if (fn && !before) tick_start();
    else if (!fn && before) tick_stop();
}
