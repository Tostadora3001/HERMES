#ifndef HERMES_ESP32C3_REGS_H
#define HERMES_ESP32C3_REGS_H

/* ESP32-C3 peripheral registers used by HERMES (ESP32-C3 Technical
 * Reference Manual; same names as the soc/..._reg.h headers of Espressif, written out
 * here: no ESP-IDF). Only what the platform and its drivers touch. */

#include <stdint.h>

#define HERMES_REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* --- UART0 (TRM ch. "UART Controller") ----------------------------------- */
#define UART0_BASE              0x60000000u
#define UART_FIFO_REG           (UART0_BASE + 0x0000) /* read: RX byte; write: TX byte */
#define UART_CLKDIV_REG         (UART0_BASE + 0x0014) /* [11:0] integer, [23:20] fraction (1/16) */
#define UART_STATUS_REG         (UART0_BASE + 0x001C) /* [9:0] RXFIFO_CNT, [25:16] TXFIFO_CNT */
#define UART_CONF0_REG          (UART0_BASE + 0x0020)
#define UART_RXFIFO_RST         (1u << 17)
#define UART_CLK_CONF_REG       (UART0_BASE + 0x0078)
#define UART_SCLK_DIV_NUM_MASK  0xFFu                 /* [7:0] */
#define UART_SCLK_SEL_SHIFT     20                    /* [21:20]: 1 APB, 2 RC_FAST, 3 XTAL */
#define UART_SCLK_SEL_XTAL      3u
#define UART_ID_REG             (UART0_BASE + 0x0080)
#define UART_REG_UPDATE         (1u << 31)            /* copy the config to the UART core */
#define UART_FIFO_SIZE          128
/* UART1: the same registers at another base (the UART_*_REG above are
 * UART0's; UART_PORT_REG(port, reg) moves one to the port's base) */
#define UART1_BASE              0x60010000u
#define UART_PORT_REG(port, reg) ((reg) - UART0_BASE + ((port) ? UART1_BASE : UART0_BASE))
/* SYSTEM: peripheral clocks and resets (UART1's are off/held at boot) */
#define SYSTEM_PERIP_CLK_EN0_REG 0x600C0010u
#define SYSTEM_PERIP_RST_EN0_REG 0x600C0018u
#define SYSTEM_UART1_BIT        (1u << 5)
/* GPIO matrix signals of UART1 (ESP32-C3 TRM, "Peripheral Signal List") */
#define GPIO_SIG_U1RXD_IN       9u
#define GPIO_SIG_U1TXD_OUT      9u
#define HERMES_ESP32C3_XTAL_HZ  40000000u

/* --- SYSTIMER: 52-bit counter at 16 MHz, running since reset ------------- */
#define SYSTIMER_BASE               0x60023000u
#define SYSTIMER_UNIT0_OP_REG       (SYSTIMER_BASE + 0x0004)
#define SYSTIMER_TIMER_UNIT0_UPDATE (1u << 30)
#define SYSTIMER_UNIT0_VALUE_VALID  (1u << 29)
#define SYSTIMER_UNIT0_VALUE_HI_REG (SYSTIMER_BASE + 0x0040)
#define SYSTIMER_UNIT0_VALUE_LO_REG (SYSTIMER_BASE + 0x0044)
#define SYSTIMER_TICKS_PER_US       16u
/* alarm 0 (the tick): compares against unit 0, periodic mode */
#define SYSTIMER_CONF_REG           (SYSTIMER_BASE + 0x0000)
#define SYSTIMER_TARGET0_WORK_EN    (1u << 24)
#define SYSTIMER_TARGET0_CONF_REG   (SYSTIMER_BASE + 0x0034)
#define SYSTIMER_TARGET0_PERIOD_MASK 0x03FFFFFFu          /* [25:0] */
#define SYSTIMER_TARGET0_PERIOD_MODE (1u << 30)
#define SYSTIMER_TARGET0_UNIT_SEL   (1u << 31)            /* 0 = unit 0 */
#define SYSTIMER_COMP0_LOAD_REG     (SYSTIMER_BASE + 0x0050)
#define SYSTIMER_INT_ENA_REG        (SYSTIMER_BASE + 0x0064)
#define SYSTIMER_INT_CLR_REG        (SYSTIMER_BASE + 0x006C)
#define SYSTIMER_TARGET0_INT        (1u << 0)

/* --- interrupt matrix (TRM ch. "Interrupt Matrix"): peripheral source ->
 * CPU interrupt 1-31; level/edge, priority 1-15 and a threshold per CPU
 * interrupt. The CPU takes interrupts in vectored mode only (mtvec). */
#define INTMTX_BASE                 0x600C2000u
#define INTMTX_MAP_REG(source)      (INTMTX_BASE + 4u * (source))
#define INTMTX_SRC_SYSTIMER_TARGET0 37u
#define INTMTX_CPU_INT_ENABLE_REG   (INTMTX_BASE + 0x0104)
#define INTMTX_CPU_INT_TYPE_REG     (INTMTX_BASE + 0x0108) /* bit n: 1 = edge, 0 = level */
#define INTMTX_CPU_INT_PRI_REG(n)   (INTMTX_BASE + 0x0114 + 4u * (n))
#define INTMTX_CPU_INT_THRESH_REG   (INTMTX_BASE + 0x0194)

/* --- GPIO (GPIO matrix) and IO_MUX --------------------------------------- */
#define GPIO_BASE                   0x60004000u
#define GPIO_OUT_W1TS_REG           (GPIO_BASE + 0x0008)
#define GPIO_OUT_W1TC_REG           (GPIO_BASE + 0x000C)
#define GPIO_ENABLE_W1TS_REG        (GPIO_BASE + 0x0024)
#define GPIO_ENABLE_W1TC_REG        (GPIO_BASE + 0x0028)
#define GPIO_IN_REG                 (GPIO_BASE + 0x003C)
#define GPIO_FUNC_OUT_SEL_CFG_REG(n) (GPIO_BASE + 0x0554 + 4u * (n))
#define GPIO_FUNC_IN_SEL_CFG_REG(sig) (GPIO_BASE + 0x0154 + 4u * (sig))
#define GPIO_SIG_IN_SEL             (1u << 6)             /* the input comes through the matrix */
#define GPIO_SIG_GPIO_OUT           0x80u                 /* output signal 128: plain GPIO */
#define GPIO_FUNC_OEN_SEL           (1u << 9)             /* output enable from GPIO_ENABLE_REG */
#define IO_MUX_BASE                 0x60009000u
#define IO_MUX_GPIO_REG(n)          (IO_MUX_BASE + 0x0004 + 4u * (n))
#define IO_MUX_FUN_WPD              (1u << 7)
#define IO_MUX_FUN_WPU              (1u << 8)
#define IO_MUX_FUN_IE               (1u << 9)
#define IO_MUX_MCU_SEL_SHIFT        12                    /* [14:12]: 1 = GPIO function */
#define IO_MUX_MCU_SEL_MASK         (7u << IO_MUX_MCU_SEL_SHIFT)
#define IO_MUX_FUNC_GPIO            1u

/* --- watchdogs ----------------------------------------------------------- */
#define HERMES_WDT_WKEY             0x50D83AA1u           /* write-protect key, every WDT */
#define TIMG0_BASE                  0x6001F000u
#define TIMG1_BASE                  0x60020000u
#define TIMG_WDTCONFIG0_REG(base)   ((base) + 0x0048)
#define TIMG_WDT_EN                 (1u << 31)
#define TIMG_WDT_CONF_UPDATE_EN     (1u << 22)
#define TIMG_WDTWPROTECT_REG(base)  ((base) + 0x0064)
#define RTC_CNTL_BASE               0x60008000u
#define RTC_CNTL_WDTCONFIG0_REG     (RTC_CNTL_BASE + 0x0090)
#define RTC_CNTL_WDTWPROTECT_REG    (RTC_CNTL_BASE + 0x00A8)
#define RTC_CNTL_SWD_CONF_REG       (RTC_CNTL_BASE + 0x00AC)
#define RTC_CNTL_SWD_AUTO_FEED_EN   (1u << 31)
#define RTC_CNTL_SWD_WPROTECT_REG   (RTC_CNTL_BASE + 0x00B0)
#define HERMES_SWD_WKEY             0x8F1D312Au

#endif
