/* Own firmware for the PY32F002A of the AVATTO ZDMS16-2.
 * Step 4: phase-angle output (TIM1) on zero crosses; with SIM_ZC=1 a virtual
 * 50 Hz zero cross drives it without mains. Tuya MCU protocol towards the Zigbee
 * module, settings kept in flash; the LED shows channel 1 (on/off), or blinks while the module is not on a
 * network. */
#include "board.h"
#include "dimmer.h"
#include "phase.h"
#include "tuya.h"
#include "uart.h"
#include "py32f0xx_ll_bus.h"
#include "py32f0xx_ll_rcc.h"
#include "py32f0xx_ll_system.h"
#include "py32f0xx_ll_utils.h"

#define NETWORK_QUERY_FIRST_MS   200
#define NETWORK_QUERY_EVERY_MS   30000

static volatile uint32_t g_ms;

void SysTick_Handler(void) {
    g_ms++;
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
    SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;
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

int main(void) {
    clock_init();
    gpio_init();
    phase_init();
    dimmer_init();
    uart_init(UART_BAUD);
    tuya_init();

    uint32_t next_query = NETWORK_QUERY_FIRST_MS;
    uint16_t query_seq  = 0;

    while (1) {
        uint32_t now = g_ms;

        tuya_poll(now);
        phase_poll();
        dimmer_poll(now, tuya_rx_quiet_ms(now), phase_save_window());

        /* Asking for the network state also tells the module we are up: it
         * answers and starts its first contact (version, DP dump). */
        if ((int32_t)(now - next_query) >= 0) {
            next_query = now + NETWORK_QUERY_EVERY_MS;
            tuya_send(TUYA_CMD_QUERY_NETWORK, query_seq++, 0, 0);
        }

        led_update(now);
    }
}
