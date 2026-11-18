/* Host build (make test) stand-in for the PY32 device header. C tests get no
 * registers at all; the flash driver test, built as C++, gets the register
 * model of flash_model.hpp (FLASH, RCC). */
#ifndef HOST_PY32F0XX_H
#define HOST_PY32F0XX_H

#include <stdint.h>

#define __disable_irq() ((void)0)
#define __enable_irq()  ((void)0)

#ifdef __cplusplus
#include "flash_model.hpp"
#else
void NVIC_SystemReset(void);   /* the test's own (records the call) */
#endif

#endif
