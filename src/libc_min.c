/* Byte-loop versions of the C library memory functions the app uses (calls
 * in the code and the compiler's own struct copies). The newlib-nano ones
 * are word-optimised, ~550 bytes together; these are ~90. Nothing here
 * handles more than a flash page (128 bytes), so the speed does not matter.
 * Built without loop-to-libcall conversion, so they cannot call themselves.
 * tests/test_libc_min.c checks them against plain reference loops. */
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) && !defined(__clang__)
#define NO_LIBCALL __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define NO_LIBCALL
#endif

NO_LIBCALL void *memset(void *d, int v, size_t n) {
    uint8_t *p = d;
    while (n--) {
        *p++ = (uint8_t)v;
    }
    return d;
}

NO_LIBCALL void *memcpy(void *d, const void *s, size_t n) {
    uint8_t       *p = d;
    const uint8_t *q = s;
    while (n--) {
        *p++ = *q++;
    }
    return d;
}

NO_LIBCALL void *memmove(void *d, const void *s, size_t n) {
    uint8_t       *p = d;
    const uint8_t *q = s;
    if (p < q) {
        while (n--) {
            *p++ = *q++;
        }
    } else {
        while (n--) {
            p[n] = q[n];
        }
    }
    return d;
}

NO_LIBCALL int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++) {
        if (*p != *q) {
            return *p - *q;
        }
    }
    return 0;
}
