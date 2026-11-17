/* PC tests for the input debouncer: make test */
#include "input_core.h"
#include <stdio.h>
#include <stdlib.h>

static int g_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            g_failed++;                                                          \
        }                                                                        \
    } while (0)

/* Feed ms samples of one level; return the first event and when (ms index). */
static uint8_t feed(input_t *in, uint8_t active, int ms, int *at) {
    for (int i = 0; i < ms; i++) {
        uint8_t ev = input_sample(in, active);
        if (ev) {
            if (at) {
                *at = i;
            }
            /* keep going so the state is complete, but report the first */
            for (int j = i + 1; j < ms; j++) {
                uint8_t ev2 = input_sample(in, active);
                if (ev2 && at) {
                    /* a second event inside one feed: tests use short feeds */
                    printf("note: second event %d at %d\n", ev2, j);
                }
            }
            return ev;
        }
    }
    return INPUT_EV_NONE;
}

static void test_press_seen_after_20_ms(void) {
    input_t in;
    input_init(&in);
    int at = -1;
    CHECK(feed(&in, 0, 100, NULL) == INPUT_EV_NONE);
    CHECK(feed(&in, 1, 30, &at) == INPUT_EV_PRESS);
    CHECK(at == INPUT_PRESS_MS - 1);         /* 20th sample = 20 ms */
}

static void test_spike_ignored(void) {
    input_t in;
    input_init(&in);
    for (int k = 0; k < 50; k++) {
        CHECK(feed(&in, 1, INPUT_PRESS_MS - 1, NULL) == INPUT_EV_NONE);   /* 19 ms */
        CHECK(feed(&in, 0, 20, NULL) == INPUT_EV_NONE);
    }
    CHECK(!in.pressed);
}

static void test_short_press_release(void) {
    input_t in;
    input_init(&in);
    int at = -1;
    feed(&in, 1, INPUT_PRESS_MS, NULL);
    CHECK(in.pressed);
    CHECK(feed(&in, 1, 150, NULL) == INPUT_EV_NONE);
    CHECK(feed(&in, 0, 40, &at) == INPUT_EV_RELEASE);
    CHECK(at == INPUT_RELEASE_MS - 1);       /* 25 ms after letting go */
}

static void test_hold_and_release_after_hold(void) {
    input_t in;
    input_init(&in);
    int at = -1;
    feed(&in, 1, INPUT_PRESS_MS, NULL);
    CHECK(feed(&in, 1, 1000, &at) == INPUT_EV_HOLD);
    CHECK(at + 1 + INPUT_PRESS_MS == INPUT_HOLD_MS);   /* hold 400 ms after the first low */
    CHECK(feed(&in, 1, 2000, NULL) == INPUT_EV_NONE);  /* reported once */
    CHECK(feed(&in, 0, 40, NULL) == INPUT_EV_RELEASE_AFTER_HOLD);
}

static void test_bounce_on_release_is_one_release(void) {
    input_t in;
    input_init(&in);
    feed(&in, 1, INPUT_PRESS_MS, NULL);
    feed(&in, 1, 100, NULL);
    /* contact bounce while opening: 1 ms open / 1 ms closed for 10 ms */
    for (int i = 0; i < 5; i++) {
        CHECK(input_sample(&in, 0) == INPUT_EV_NONE);
        CHECK(input_sample(&in, 1) == INPUT_EV_NONE);
    }
    CHECK(feed(&in, 0, 40, NULL) == INPUT_EV_RELEASE);
    CHECK(feed(&in, 0, 200, NULL) == INPUT_EV_NONE);
}

/* Short ghost pulses on an open switch (leakage on long wires): never a press. */
static void test_ghost_pulses_are_not_a_press(void) {
    input_t in;
    input_init(&in);
    for (int cyc = 0; cyc < 200; cyc++) {
        for (int ms = 0; ms < 20; ms++) {
            CHECK(input_sample(&in, ms < 8) == INPUT_EV_NONE);
        }
    }
    CHECK(!in.pressed);
}

/* Let go just before the hold time: a short press, not hold + release. */
static void test_release_just_before_hold_is_a_short_press(void) {
    input_t in;
    input_init(&in);
    feed(&in, 1, INPUT_PRESS_MS, NULL);
    CHECK(feed(&in, 1, INPUT_HOLD_MS - INPUT_PRESS_MS - 10, NULL) == INPUT_EV_NONE);
    int     at = -1;
    uint8_t ev = feed(&in, 0, 40, &at);
    CHECK(ev == INPUT_EV_RELEASE);
}

int main(void) {
    test_press_seen_after_20_ms();
    test_spike_ignored();
    test_short_press_release();
    test_hold_and_release_after_hold();
    test_bounce_on_release_is_one_release();
    test_ghost_pulses_are_not_a_press();
    test_release_just_before_hold_is_a_short_press();
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    printf("input_core: all tests passed\n");
    return EXIT_SUCCESS;
}
