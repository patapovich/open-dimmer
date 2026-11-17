/* Own firmware for the PY32F002A of the AVATTO ZDMS16-2.
 * Step 4: phase-angle output (TIM1) on zero crosses; with SIM_ZC=1 a virtual
 * 50 Hz zero cross drives it without mains. Tuya MCU protocol towards the Zigbee
 * module, settings kept in flash, wall switch inputs and the module button; the LED shows channel 1 (on/off), or blinks while the module is not on a
 * network. */
#include "board.h"
#include <string.h>
#include "controls.h"
#include "dimmer.h"
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

/* Found by the bootloader right after the vector table; size and CRC are
 * filled in by tools/mkimage.py. */
__attribute__((section(".app_header"), used))
const app_header_t g_app_header = { APP_MAGIC, 0, 0, FW_VERSION,
                                    (SIM_ZC ? APP_FLAG_SIM : 0) | (MEASURE ? APP_FLAG_MEASURE : 0), { 0, 0 } };

static volatile uint32_t g_ms;

void SysTick_Handler(void) {
    g_ms++;
    controls_tick_1ms();
}

static void clock_init(void) {
    LL_RCC_HSI_Enable();
    LL_RCC_HSI_SetCalibFreq(LL_RCC_HSICALIBRATION_24MHz);
    while (LL_RCC_HSI_IsReady() != 1) {
    }
    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_HSISYS);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_HSISYS) {
    }
    LL_FLASH_SetLatency(LL_FLASH_LATENCY_0);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
    LL_SetSystemCoreClock(24000000);
    LL_Init1msTick(24000000);
    NVIC_SetPriority(SysTick_IRQn, 2);   /* zero cross 0, UART 1 */
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

static void led_update(uint32_t now) {
    uint8_t on;

    if (tuya_network_status() != TUYA_NETWORK_CONNECTED) {
        on = (now / 250) & 1;              /* not on a network (or unknown): blink */
    } else {
        on = dimmer_channel(0)->on;
    }
    if (on) {
        LL_GPIO_ResetOutputPin(LED_PORT, LED_PIN);   /* active low */
    } else {
        LL_GPIO_SetOutputPin(LED_PORT, LED_PIN);
    }
}

#if MEASURE
/* Measurement build: every 10 s the statistics go to the module as reports of
 * DPs 110..119, which its MCU link diagnostics log (Basic 0xff41..0xff45):
 * readable from Zigbee2MQTT with nothing wired to the board.
 *   110/111/112 PA4 high: average / min / max us    113/114/115 low: same
 *   116 edges, 117 glitches (< 2 ms) in the window   118/119 S1/S2 low time ms */
#define MEASURE_EVERY_MS   10000
static uint32_t g_measure_next = MEASURE_EVERY_MS;
static uint16_t g_s_low_ms[2];

static void measure_poll(uint32_t now) {
    static uint32_t last_ms;
    if (now != last_ms) {                      /* switch inputs, sampled once a ms */
        last_ms = now;
        g_s_low_ms[0] += !LL_GPIO_IsInputPinSet(S1_PORT, S1_PIN);
        g_s_low_ms[1] += !LL_GPIO_IsInputPinSet(S2_PORT, S2_PIN);
    }
    if ((int32_t)(now - g_measure_next) < 0) {
        return;
    }
    g_measure_next = now + MEASURE_EVERY_MS;
    zc_measure_t m;
    __disable_irq();
    m = *(zc_measure_t *)&g_zc_measure;
    memset((void *)&g_zc_measure, 0, sizeof(g_zc_measure));
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
    tuya_report_send();
    g_s_low_ms[0] = 0;
    g_s_low_ms[1] = 0;
}
#endif

/* Independent watchdog, ~1 s (LSI 32 kHz / 32, reload 1000). Once started it
 * runs through software resets too: the bootloader keeps refreshing it. */
static void watchdog_start(void) {
    /* The PY32 does not start its LSI for the watchdog by itself. (Frozen
     * under a debugger only if the debugger sets DBGMCU APBFZ1 bit 12: the
     * app cannot reach DBGMCU; `make flash` does it.) */
    RCC->CSR |= RCC_CSR_LSION;
    while (!(RCC->CSR & RCC_CSR_LSIRDY)) {
    }
    IWDG->KR  = 0xCCCC;
    IWDG->KR  = 0x5555;
    IWDG->PR  = 3;
    IWDG->RLR = 1000;
    while (IWDG->SR) {
    }
    IWDG->KR = 0xAAAA;
}

int main(void) {
    clock_init();
    gpio_init();
    phase_init();
    dimmer_init();
    controls_init();
    SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;
    uart_init(UART_BAUD);
    tuya_init();
    watchdog_start();

    uint32_t next_query = NETWORK_QUERY_FIRST_MS;
    uint16_t query_seq  = 0;

    while (1) {
        uint32_t now = g_ms;

        IWDG->KR = 0xAAAA;

        tuya_poll(now);
        phase_poll();
        controls_poll();
        dimmer_poll(now, tuya_rx_quiet_ms(now), phase_save_window());

        /* Asking for the network state also tells the module we are up: it
         * answers and starts its first contact (version, DP dump). */
        if ((int32_t)(now - next_query) >= 0) {
            next_query = now + (tuya_network_status() == TUYA_NETWORK_CONNECTED
                                ? NETWORK_QUERY_EVERY_MS : NETWORK_QUERY_JOINING_MS);
            tuya_send(TUYA_CMD_QUERY_NETWORK, query_seq++, 0, 0);
        }

        led_update(now);
#if MEASURE
        measure_poll(now);
#endif
    }
}
