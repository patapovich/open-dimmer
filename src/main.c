/* Own firmware for the PY32F002A of the AVATTO ZDMS16-2.
 * Step 4: phase-angle output (TIM1) on zero crosses; with SIM_ZC=1 a virtual
 * 50 Hz zero cross drives it without mains. Tuya MCU protocol towards the Zigbee
 * module, settings kept in flash, wall switch inputs and the module button; the
 * LED is lit while any channel is on, and blinks while the module is not on a
 * network (the diagnostics build mirrors PA4 on it instead). */
#include "board.h"
#include <string.h>
#include "bootif.h"
#include "controls.h"
#include "dimmer.h"
#include "health.h"
#include "ota_proto.h"
#include "phase.h"
#include "tuya.h"
#include "uart.h"
#include "py32f0xx_ll_bus.h"
#include "py32f0xx_ll_rcc.h"
#include "py32f0xx_ll_system.h"
#include "py32f0xx_ll_utils.h"

#define NETWORK_QUERY_FIRST_MS   200
#define NETWORK_QUERY_EVERY_MS   30000
#define NETWORK_QUERY_JOINING_MS 2000    /* until on a network: the LED follows soon */

/* Settings ring at 0x08004800 and images up to 14 KB: only from the ring-move release M on
 * (docs/BOOTLOADER.md section 1), with its linker layout and store scan rule. */
#ifndef RING_HIGH
#define RING_HIGH 0                /* set by the Makefile with the linker layout */
#endif
#if (RING_HIGH != 0) != (FW_BUILD_NUMBER >= 400)
#error "RING_HIGH exactly from release M (build 400) on: builds below it keep the low layout"
#endif

/* Found by the bootloader right after the vector table; size and CRC are
 * filled in by tools/mkimage.py. Every build confirms its runs (bootif.c);
 * the v2 bootloader counts the unconfirmed ones of normal builds only. */
__attribute__((section(".app_header"), used))
const app_header_t g_app_header = { APP_MAGIC, 0, 0, FW_VERSION,
                                    APP_FLAG_CONFIRMS | (RING_HIGH ? APP_FLAG_RING_HIGH : 0) |
                                    (SIM_ZC ? APP_FLAG_SIM : 0) | (MEASURE ? APP_FLAG_MEASURE : 0) |
                                    (DIAG_REPORT ? APP_FLAG_DIAG : 0), FW_IMAGE_BUILD };
_Static_assert(!SIM_ZC || FW_IMAGE_BUILD == OTA_BUILD_SIM, "SIM builds: build 0xFFFE");
_Static_assert(SIM_ZC || (FW_IMAGE_BUILD >= OTA_BUILD_MIN && FW_IMAGE_BUILD <= OTA_BUILD_MAX),
               "build number");

static volatile uint32_t g_ms;

void SysTick_Handler(void) {
    g_ms++;
    controls_tick_1ms();
}

/* HSI at SYSCLK_HZ with the factory trim, the 1 ms SysTick. The register writes of
 * LL_RCC_HSI_SetCalibFreq() and LL_Init1msTick(), with the clock as a constant: the first
 * also runs SystemCoreClockUpdate() (with its tables), the second divides at run time, and
 * the fixed clock makes both moot (-244 bytes, the same register values). */
static void clock_init(void) {
    LL_RCC_HSI_Enable();
    MODIFY_REG(RCC->ICSCR, RCC_ICSCR_HSI_FS | RCC_ICSCR_HSI_TRIM, LL_RCC_HSICALIBRATION_24MHz);
    while (LL_RCC_HSI_IsReady() != 1) {
    }
    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_HSISYS);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_HSISYS) {
    }
    LL_FLASH_SetLatency(LL_FLASH_LATENCY_0);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
    SystemCoreClock = SYSCLK_HZ;
    LL_InitTick(SYSCLK_HZ, 1000u);
    NVIC_SetPriority(SysTick_IRQn, IRQ_PRIO_TICK);   /* below the UART and the zero cross (board.h) */
    /* The tick interrupt (input sampling) starts after controls_init. */
}

static void out_low(GPIO_TypeDef *port, uint32_t pin) {
    LL_GPIO_ResetOutputPin(port, pin);
    LL_GPIO_SetPinOutputType(port, pin, LL_GPIO_OUTPUT_PUSHPULL);
    LL_GPIO_SetPinMode(port, pin, LL_GPIO_MODE_OUTPUT);
}

static void gpio_init(void) {
    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA | LL_IOP_GRP1_PERIPH_GPIOB);

    /* Output stage off first. */
    out_low(GATE1_PORT, GATE1_PIN);
    out_low(GATE2_PORT, GATE2_PIN);
    LL_GPIO_SetOutputPin(LED_PORT, LED_PIN);   /* LED off (active low) */
    LL_GPIO_SetPinOutputType(LED_PORT, LED_PIN, LL_GPIO_OUTPUT_PUSHPULL);
    LL_GPIO_SetPinMode(LED_PORT, LED_PIN, LL_GPIO_MODE_OUTPUT);

    LL_GPIO_SetPinMode(BUTTON_PORT, BUTTON_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(BUTTON_PORT, BUTTON_PIN, LL_GPIO_PULL_UP);
    LL_GPIO_SetPinMode(S1_PORT, S1_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(S1_PORT, S1_PIN, LL_GPIO_PULL_UP);
    LL_GPIO_SetPinMode(S2_PORT, S2_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(S2_PORT, S2_PIN, LL_GPIO_PULL_UP);
    LL_GPIO_SetPinMode(ZC_PORT, ZC_PIN, LL_GPIO_MODE_INPUT);
}

#if !DIAG_REPORT
static void led_update(uint32_t now) {
    uint8_t on;

    if (tuya_network_status() != TUYA_NETWORK_CONNECTED) {
        on = (now / 250) & 1;              /* not on a network (or unknown): blink */
    } else {
        on = dimmer_channel(0)->on || dimmer_channel(1)->on;
    }
    if (on) {
        LL_GPIO_ResetOutputPin(LED_PORT, LED_PIN);   /* active low */
    } else {
        LL_GPIO_SetOutputPin(LED_PORT, LED_PIN);
    }
}
#endif

#if MEASURE || DIAG_REPORT
/* Measurement / diagnostics builds: every 10 s the statistics go to the module
 * as reports of DPs 110..123, which its MCU link diagnostics log (Basic
 * 0xff41..0xff45): readable from Zigbee2MQTT with nothing wired to the board.
 *   110/111/112 PA4 high: average / min / max us    113/114/115 low: same
 *   116 edges, 117 glitches (< 2 ms) in the window   118/119 S1/S2 low time ms
 *   120/121 S1/S2 level changes                      122 lock losses, 123 edges ignored */
#define MEASURE_EVERY_MS   10000
static uint32_t g_measure_next = MEASURE_EVERY_MS;
static uint16_t g_s_low_ms[2];
static uint16_t g_s_changes[2];
static void measure_poll(uint32_t now) {
    static uint32_t last_ms;
    static uint8_t  last_level[2] = { 1, 1 };
    if (now != last_ms) {                      /* switch inputs, sampled once a ms */
        last_ms = now;
        uint8_t lv[2] = { LL_GPIO_IsInputPinSet(S1_PORT, S1_PIN) ? 1u : 0u,
                          LL_GPIO_IsInputPinSet(S2_PORT, S2_PIN) ? 1u : 0u };
        for (uint8_t i = 0; i < 2; i++) {
            g_s_low_ms[i] += (uint16_t)!lv[i];
            if (lv[i] != last_level[i]) {
                g_s_changes[i]++;
                last_level[i] = lv[i];
            }
        }
    }
    if ((int32_t)(now - g_measure_next) < 0) {
        return;
    }
    g_measure_next = now + MEASURE_EVERY_MS;
    zc_measure_t m;
    __disable_irq();
    m = *(zc_measure_t *)&g_zc_measure;
    memset((void *)&g_zc_measure, 0, sizeof(g_zc_measure));
    uint32_t losses  = g_phase_dbg.lock_losses;
    uint32_t ignored = g_phase_dbg.zc_ignored;
    __enable_irq();
    tuya_report_begin(1);
    tuya_report_value(110, m.high.count ? m.high.sum * 20u / m.high.count : 0);
    tuya_report_value(111, m.high.min * 20u);
    tuya_report_value(112, m.high.max * 20u);
    tuya_report_value(113, m.low.count ? m.low.sum * 20u / m.low.count : 0);
    tuya_report_value(114, m.low.min * 20u);
    tuya_report_value(115, m.low.max * 20u);
    tuya_report_value(116, m.edges);
    tuya_report_value(117, m.glitches);
    tuya_report_value(118, g_s_low_ms[0]);
    tuya_report_value(119, g_s_low_ms[1]);
    tuya_report_value(120, g_s_changes[0]);
    tuya_report_value(121, g_s_changes[1]);
    tuya_report_value(122, losses);
    tuya_report_value(123, ignored);
    tuya_report_send();
    g_s_low_ms[0]  = 0;
    g_s_low_ms[1]  = 0;
    g_s_changes[0] = 0;
    g_s_changes[1] = 0;
}
#endif

/* Independent watchdog, ~1 s (LSI 32 kHz / 32, reload 1000), the period the v2
 * bootloader sets at every boot (it is running already then). Started first,
 * so a hang anywhere in the init resets too. The waits are bounded like the
 * bootloader's: with an LSI that never starts the watchdog does not run, but
 * the light does. */
static void watchdog_start(void) {
    /* The PY32 does not start its LSI for the watchdog by itself. (Frozen
     * under a debugger only if the debugger sets DBGMCU APBFZ1 bit 12: the
     * app cannot reach DBGMCU; `make flash` does it.) */
    RCC->CSR |= RCC_CSR_LSION;
    for (uint32_t n = 100000u; n && !(RCC->CSR & RCC_CSR_LSIRDY); n--) {
    }
    IWDG->KR  = 0xCCCC;
    IWDG->KR  = 0x5555;
    IWDG->PR  = 3;
    IWDG->RLR = 1000;
    for (uint32_t n = 100000u; n && IWDG->SR; n--) {
    }
    IWDG->KR = 0xAAAA;
}

/* A fault restarts at once instead of spinning in the SDK's default handler
 * until the watchdog bites (~1 s, the light dark): the light comes back as it
 * was (dimmer_init). Unused interrupts still go to the default handler, which
 * is not weak; the watchdog ends that. */
void HardFault_Handler(void) {
    SCB->AIRCR = (0x5FAu << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
    for (;;) {
    }
}
void NMI_Handler(void) __attribute__((alias("HardFault_Handler")));

int main(void) {
    uint32_t csr = RCC->CSR;                    /* why we (re)started, before the flags are cleared */
    RCC->CSR |= RCC_CSR_RMVF;
    watchdog_start();                           /* before anything that could hang */
#if !(MEASURE || DIAG_REPORT)
    health_init(csr >> 24);                     /* kept for the health report */
#endif
    clock_init();
    gpio_init();
    phase_init();
    bootif_init();                              /* the bootloader: boot_id, boot word, sector CRC */
    _Static_assert(DIMMER_RESET_POWER == RCC_CSR_PWRRSTF && DIMMER_RESET_SOFT == RCC_CSR_SFTRSTF &&
                   DIMMER_RESET_WDG == RCC_CSR_IWDGRSTF, "reset flags");
    controls_init(dimmer_init(csr));            /* watchdog, fault, update: back as it was */
    SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;
    uart_init();
    tuya_init();

    uint32_t next_query = NETWORK_QUERY_FIRST_MS;

    while (1) {
        uint32_t now = g_ms;

        IWDG->KR = 0xAAAA;

        tuya_poll(now);
        phase_poll();
        controls_poll(now);
        dimmer_poll(now, tuya_rx_quiet_ms(now), phase_save_window());

        /* Asking for the network state also tells the module we are up: it
         * answers and starts its first contact (version, DP dump). */
        if ((int32_t)(now - next_query) >= 0) {
            next_query = now + (tuya_network_status() == TUYA_NETWORK_CONNECTED
                                ? NETWORK_QUERY_EVERY_MS : NETWORK_QUERY_JOINING_MS);
            tuya_query_network(now);
        }

#if !DIAG_REPORT
        led_update(now);                         /* diagnostics build: the LED mirrors PA4 */
#endif
#if !(MEASURE || DIAG_REPORT)
        health_poll(now);
#endif
        bootif_poll(now);                        /* confirmation; link watchdog (may restart) */
#if MEASURE || DIAG_REPORT
        measure_poll(now);
#endif
    }
}
