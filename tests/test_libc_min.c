/* PC tests for src/libc_min.c, the app's byte-loop memset/memcpy/memmove/
 * memcmp: built with the names changed (lm_*), so the host C library is not
 * replaced, and compared against plain reference loops. make test */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void *lm_memset(void *d, int v, size_t n);
void *lm_memcpy(void *d, const void *s, size_t n);
void *lm_memmove(void *d, const void *s, size_t n);
int   lm_memcmp(const void *a, const void *b, size_t n);

static int g_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            g_failed++;                                                          \
        }                                                                        \
    } while (0)

#define BUF 320

static uint32_t g_rand = 12345;
static uint8_t  rnd8(void) {
    g_rand = g_rand * 1103515245u + 12345u;
    return (uint8_t)(g_rand >> 16);
}

static void fill(uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        p[i] = rnd8();
    }
}

static int same(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int sign(int v) {
    return (v > 0) - (v < 0);
}

/* Every length up to past a flash page, every alignment of both ends. */
static void test_memset_memcpy(void) {
    uint8_t buf[BUF], exp[BUF], src[BUF];
    for (size_t n = 0; n <= 140; n++) {
        for (size_t da = 0; da < 4; da++) {
            for (size_t sa = 0; sa < 4; sa++) {
                fill(buf, BUF);
                fill(src, BUF);
                for (size_t i = 0; i < BUF; i++) {
                    exp[i] = buf[i];
                }
                for (size_t i = 0; i < n; i++) {
                    exp[da + i] = src[sa + i];
                }
                CHECK(lm_memcpy(buf + da, src + sa, n) == buf + da);
                CHECK(same(buf, exp, BUF));

                int v = (int)(n * 37 + da) - 300;          /* only the low byte counts */
                for (size_t i = 0; i < n; i++) {
                    exp[da + i] = (uint8_t)v;
                }
                CHECK(lm_memset(buf + da, v, n) == buf + da);
                CHECK(same(buf, exp, BUF));
            }
        }
    }
}

/* Overlap in both directions (the RX buffer shifts down), and no overlap. */
static void test_memmove(void) {
    uint8_t buf[BUF], exp[BUF], tmp[BUF];
    for (size_t n = 0; n <= 140; n += 1) {
        for (size_t d = 0; d <= 160; d += 7) {
            for (size_t s = 0; s <= 160; s += 5) {
                fill(buf, BUF);
                for (size_t i = 0; i < BUF; i++) {
                    exp[i] = buf[i];
                }
                for (size_t i = 0; i < n; i++) {
                    tmp[i] = buf[s + i];
                }
                for (size_t i = 0; i < n; i++) {
                    exp[d + i] = tmp[i];
                }
                CHECK(lm_memmove(buf + d, buf + s, n) == buf + d);
                CHECK(same(buf, exp, BUF));
            }
        }
    }
}

static void test_memcmp(void) {
    uint8_t a[BUF], b[BUF];
    for (size_t n = 0; n <= 140; n++) {
        fill(a, n);
        for (size_t i = 0; i < n; i++) {
            b[i] = a[i];
        }
        CHECK(lm_memcmp(a, b, n) == 0);
        for (size_t k = 0; k < n; k += 3) {             /* first difference decides */
            uint8_t keep = b[k];
            b[k] = (uint8_t)(a[k] + 1 + rnd8() % 255);
            if (k + 1 < n) {
                b[k + 1] = (uint8_t)~a[k + 1];          /* a later difference must not count */
            }
            CHECK(sign(lm_memcmp(a, b, n)) == (a[k] > b[k] ? 1 : -1));
            CHECK(sign(lm_memcmp(b, a, n)) == (b[k] > a[k] ? 1 : -1));
            CHECK(lm_memcmp(a, b, k) == 0);              /* stops at n */
            b[k] = keep;
            if (k + 1 < n) {
                b[k + 1] = a[k + 1];
            }
        }
    }
    uint8_t lo = 0x01, hi = 0xFF;                       /* bytes compare unsigned */
    CHECK(lm_memcmp(&lo, &hi, 1) < 0);
    CHECK(lm_memcmp(&hi, &lo, 1) > 0);
}

int main(void) {
    test_memset_memcpy();
    test_memmove();
    test_memcmp();
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    printf("libc_min: all tests passed\n");
    return EXIT_SUCCESS;
}
