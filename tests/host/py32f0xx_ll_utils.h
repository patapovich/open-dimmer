/* Host build stand-in: no delay. */
#ifndef HOST_PY32F0XX_LL_UTILS_H
#define HOST_PY32F0XX_LL_UTILS_H

#include <stdint.h>

static inline void LL_mDelay(uint32_t ms) {
    (void)ms;
}

#endif
