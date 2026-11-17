#include "gesture_core.h"
#include "input_core.h"
#include <stdio.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* Short presses at the given times (press, release 120 ms later); returns what
 * polling every ms up to `until` reported, and when. */
static uint8_t run(const uint32_t *presses, int n, uint32_t until, uint32_t *at) {
    gesture_t g;
    gesture_init(&g);
    uint8_t got = GESTURE_NONE;
    int     k   = 0;
    for (uint32_t t = 0; t <= until; t++) {
        if (k < n && t == presses[k]) {
            CHECK(gesture_event(&g, INPUT_EV_PRESS, t) == GESTURE_NONE);
        }
        if (k < n && t == presses[k] + 120) {
            CHECK(gesture_event(&g, INPUT_EV_RELEASE, t) == GESTURE_NONE);
            k++;
        }
        uint8_t r = gesture_poll(&g, t);
        if (r != GESTURE_NONE) {
            CHECK(got == GESTURE_NONE);    /* one gesture per sequence */
            got = r;
            *at = t;
        }
    }
    return got;
}

static void test_counts(void) {
    uint32_t at = 0;
    const uint32_t one[]   = { 10 };
    const uint32_t two[]   = { 10, 300 };
    const uint32_t three[] = { 10, 300, 600 };
    const uint32_t four[]  = { 10, 300, 600, 900 };
    CHECK(run(one, 1, 2000, &at) == GESTURE_SINGLE);
    CHECK(at == 130 + GESTURE_GAP_MS);                 /* gap after the release */
    CHECK(run(two, 2, 2000, &at) == GESTURE_DOUBLE);
    CHECK(at == 420 + GESTURE_GAP_MS);
    CHECK(run(three, 3, 2000, &at) == GESTURE_TRIPLE);
    CHECK(run(four, 4, 3000, &at) == GESTURE_NONE);    /* not meant */
}

static void test_gap_splits_sequences(void) {
    gesture_t g;
    gesture_init(&g);
    gesture_event(&g, INPUT_EV_PRESS, 0);
    gesture_event(&g, INPUT_EV_RELEASE, 100);
    CHECK(gesture_poll(&g, 100 + GESTURE_GAP_MS - 1) == GESTURE_NONE);
    CHECK(gesture_poll(&g, 100 + GESTURE_GAP_MS) == GESTURE_SINGLE);
    CHECK(gesture_poll(&g, 2000) == GESTURE_NONE);     /* reported once */
    gesture_event(&g, INPUT_EV_PRESS, 3000);           /* a new sequence */
    gesture_event(&g, INPUT_EV_RELEASE, 3100);
    CHECK(gesture_poll(&g, 3100 + GESTURE_GAP_MS) == GESTURE_SINGLE);
}

static void test_no_report_while_down(void) {
    gesture_t g;
    gesture_init(&g);
    gesture_event(&g, INPUT_EV_PRESS, 0);
    CHECK(gesture_poll(&g, 390) == GESTURE_NONE);      /* still pressed, not yet a hold */
}

static void test_hold(void) {
    gesture_t g;
    gesture_init(&g);
    gesture_event(&g, INPUT_EV_PRESS, 0);
    gesture_event(&g, INPUT_EV_RELEASE, 100);
    gesture_event(&g, INPUT_EV_PRESS, 300);            /* tap then hold */
    CHECK(gesture_event(&g, INPUT_EV_HOLD, 700) == GESTURE_HOLD);
    CHECK(gesture_poll(&g, 1500) == GESTURE_NONE);     /* the tap is dropped */
    CHECK(gesture_event(&g, INPUT_EV_RELEASE_AFTER_HOLD, 2500) == GESTURE_RELEASE);
    CHECK(gesture_poll(&g, 4000) == GESTURE_NONE);
}

static void test_time_wraps(void) {
    gesture_t g;
    gesture_init(&g);
    uint32_t t0 = 0xFFFFFF00u;
    gesture_event(&g, INPUT_EV_PRESS, t0);
    gesture_event(&g, INPUT_EV_RELEASE, t0 + 100);
    CHECK(gesture_poll(&g, t0 + 100 + GESTURE_GAP_MS - 1) == GESTURE_NONE);
    CHECK(gesture_poll(&g, t0 + 100 + GESTURE_GAP_MS) == GESTURE_SINGLE);   /* past 0 */
}

int main(void) {
    test_counts();
    test_gap_splits_sequences();
    test_no_report_while_down();
    test_hold();
    test_time_wraps();
    if (!g_fail) {
        printf("gesture_core: all tests passed\n");
    }
    return g_fail;
}
