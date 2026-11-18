/* Host build stand-in: board.h names pins. The ports are plain input
 * registers that a test sets (tests/test_controls.c); the pin setters do
 * nothing. */
#ifndef HOST_PY32F0XX_LL_GPIO_H
#define HOST_PY32F0XX_LL_GPIO_H

#include <stdint.h>
#include "py32f0xx.h"

typedef struct {
    uint32_t IDR;
} GPIO_TypeDef;
extern GPIO_TypeDef g_host_gpioa, g_host_gpiob;   /* tests/host_stubs.c */

#define GPIOA              (&g_host_gpioa)
#define GPIOB              (&g_host_gpiob)
#define LL_GPIO_PIN_0      (1u << 0)
#define LL_GPIO_PIN_1      (1u << 1)
#define LL_GPIO_PIN_2      (1u << 2)
#define LL_GPIO_PIN_3      (1u << 3)
#define LL_GPIO_PIN_4      (1u << 4)
#define LL_GPIO_PIN_5      (1u << 5)
#define LL_GPIO_PIN_6      (1u << 6)
#define LL_GPIO_SetPinMode(...)    ((void)0)
#define LL_GPIO_SetPinSpeed(...)   ((void)0)
#define LL_GPIO_SetPinPull(...)    ((void)0)
#define LL_GPIO_SetAFPin_0_7(...)  ((void)0)

static inline uint32_t LL_GPIO_IsInputPinSet(const GPIO_TypeDef *port, uint32_t pin) {
    return (port->IDR & pin) == pin;
}

#endif
