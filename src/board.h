/* AVATTO ZDMS16-2 board, PY32F002A (TSSOP20). Pin map from the analysis of the
 * stock firmware of the twin WiFi board (DMS16-W1), see docs/stock-firmware-analysis.md. */
#ifndef BOARD_H
#define BOARD_H

#ifndef HOST_TEST
#include "py32f0xx_ll_gpio.h"
#else
/* Host tests (tests/): ports are plain input registers that the test sets;
 * the few LL and CMSIS calls the pin users make. */
#include <stdint.h>
typedef struct {
    uint32_t IDR;
} GPIO_TypeDef;
extern GPIO_TypeDef g_host_gpioa, g_host_gpiob;
#define GPIOA           (&g_host_gpioa)
#define GPIOB           (&g_host_gpiob)
#define LL_GPIO_PIN_0   (1u << 0)
#define LL_GPIO_PIN_1   (1u << 1)
#define LL_GPIO_PIN_2   (1u << 2)
#define LL_GPIO_PIN_3   (1u << 3)
#define LL_GPIO_PIN_4   (1u << 4)
#define LL_GPIO_PIN_5   (1u << 5)
#define LL_GPIO_PIN_6   (1u << 6)
static inline uint32_t LL_GPIO_IsInputPinSet(const GPIO_TypeDef *port, uint32_t pin) {
    return (port->IDR & pin) == pin;
}
static inline void LL_mDelay(uint32_t ms) {
    (void)ms;
}
#define __disable_irq() ((void)0)
#define __enable_irq()  ((void)0)
#endif

/* Output stage gate drives (TIM1, AF13 in the stock firmware). Active high:
 * high = load conducts. Must be low whenever the firmware is not in control. */
#define GATE1_PORT      GPIOA
#define GATE1_PIN       LL_GPIO_PIN_1   /* channel 1, TIM1_CH4 */
#define GATE2_PORT      GPIOA
#define GATE2_PIN       LL_GPIO_PIN_0   /* channel 2, TIM1_CH3 */

/* Mains zero cross, EXTI on both edges */
#define ZC_PORT         GPIOA
#define ZC_PIN          LL_GPIO_PIN_4

/* LED (push-pull output in the stock firmware; blinks while pairing).
 * Active low: lit when the pin is low (seen on the spare 2026-11-17). */
#define LED_PORT        GPIOA
#define LED_PIN         LL_GPIO_PIN_5

/* Inputs, pull-up, active low */
#define BUTTON_PORT     GPIOA
#define BUTTON_PIN      LL_GPIO_PIN_6   /* module button */
#define S1_PORT         GPIOB
#define S1_PIN          LL_GPIO_PIN_1   /* wall switch input 1 */
#define S2_PORT         GPIOB
#define S2_PIN          LL_GPIO_PIN_2   /* wall switch input 2 */

/* UART to the Zigbee module (ZTU): USART1, AF1, 115200 8N1 */
#define UART_TX_PORT    GPIOA
#define UART_TX_PIN     LL_GPIO_PIN_2
#define UART_RX_PORT    GPIOA
#define UART_RX_PIN     LL_GPIO_PIN_3
#define UART_BAUD       115200

#endif
