/* Step 1: bring-up. Blink the LED, keep the output stage off, and report DP1
 * toggling every 2 s to the Zigbee module so the whole chain
 * PY32 -> UART -> ZTU -> Zigbee2MQTT can be checked without mains. */
#include "board.h"
#include "tuya.h"
#include "py32f0xx_ll_bus.h"
#include "py32f0xx_ll_rcc.h"
#include "py32f0xx_ll_system.h"
#include "py32f0xx_ll_utils.h"

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
    out_low(LED_PORT, LED_PIN);

    LL_GPIO_SetPinMode(BUTTON_PORT, BUTTON_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(BUTTON_PORT, BUTTON_PIN, LL_GPIO_PULL_UP);
    LL_GPIO_SetPinMode(S1_PORT, S1_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(S1_PORT, S1_PIN, LL_GPIO_PULL_UP);
    LL_GPIO_SetPinMode(S2_PORT, S2_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(S2_PORT, S2_PIN, LL_GPIO_PULL_UP);
    LL_GPIO_SetPinMode(ZC_PORT, ZC_PIN, LL_GPIO_MODE_INPUT);
}

int main(void) {
    clock_init();
    gpio_init();
    tuya_init();

    uint32_t next_blink  = 0;
    uint32_t next_report = 2000;
    uint8_t  dp1         = 0;

    while (1) {
        uint32_t now = g_ms;

        if ((int32_t)(now - next_blink) >= 0) {
            next_blink += 500;
            LL_GPIO_TogglePin(LED_PORT, LED_PIN);
        }
        if ((int32_t)(now - next_report) >= 0) {
            next_report += 2000;
            dp1 ^= 1;
            tuya_report_bool(1, dp1);
        }
    }
}
