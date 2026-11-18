/* Host build stand-in: board.h names pins, the host tests drive no GPIO. */
#ifndef HOST_PY32F0XX_LL_GPIO_H
#define HOST_PY32F0XX_LL_GPIO_H

#include <stdint.h>

#define GPIOA              ((void *)0)
#define GPIOB              ((void *)0)
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

#endif
