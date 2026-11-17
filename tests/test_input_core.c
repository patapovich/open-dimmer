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

static void test_press_seen_within_3_ms(void) {
    input_t in;
    input_init(&in);
    int at = -1;
    CHECK(feed(&in, 0, 100, NULL) == INPUT_EV_NONE);
    CHECK(feed(&in, 1, 10, &at) == INPUT_EV_PRESS);
    CHECK(at == INPUT_PRESS_MS - 1);         /* 3rd sample = 3 ms */
}

static void test_spike_ignored(void) {
    input_t in;
    input_init(&in);
    for (int k = 0; k < 50; k++) {
        CHECK(feed(&in, 1, 2, NULL) == INPUT_EV_NONE);   /* 2 ms noise */
        CHECK(feed(&in, 0, 20, NULL) == INPUT_EV_NONE);
    }
    CHECK(!in.pressed);
}

static void test_short_press_release(void) {
    input_t in;
    input_init(&in);
    int at = -1;
    feed(&in, 1, 3, NULL);
    CHECK(in.pressed);
    CHECK(feed(&in, 1, 150, NULL) == INPUT_EV_NONE);
    CHECK(feed(&in, 0, 40, &at) == INPUT_EV_RELEASE);
    CHECK(at == INPUT_RELEASE_MS - 1);       /* 25 ms after letting go */
}

static void test_hold_and_release_after_hold(void) {
    input_t in;
    input_init(&in);
    int at = -1;
    feed(&in, 1, 3, NULL);
    CHECK(feed(&in, 1, 1000, &at) == INPUT_EV_HOLD);
    CHECK(at + 1 + INPUT_PRESS_MS == INPUT_HOLD_MS);   /* hold 400 ms after the first low */
    CHECK(feed(&in, 1, 2000, NULL) == INPUT_EV_NONE);  /* reported once */
    CHECK(feed(&in, 0, 40, NULL) == INPUT_EV_RELEASE_AFTER_HOLD);
}

static void test_bounce_on_release_is_one_release(void) {
    input_t in;
    input_init(&in);
    feed(&in, 1, 3, NULL);
    feed(&in, 1, 100, NULL);
    /* contact bounce while opening: 1 ms open / 1 ms closed for 10 ms */
    for (int i = 0; i < 5; i++) {
        CHECK(input_sample(&in, 0) == INPUT_EV_NONE);
        CHECK(input_sample(&in, 1) == INPUT_EV_NONE);
    }
    CHECK(feed(&in, 0, 40, NULL) == INPUT_EV_RELEASE);
    CHECK(feed(&in, 0, 200, NULL) == INPUT_EV_NONE);
}

/* If S1/S2 come through an optocoupler from the mains, a closed switch is a
 * 50 Hz pulse train: low ~8 ms, high ~12 ms (half-wave). Must read as one
 * continuous press, and the release must still be quick. */
static void test_50hz_pulse_train_is_one_press(void) {
    input_t in;
    input_init(&in);
    uint8_t presses = 0, releases = 0, holds = 0;
    for (int cyc = 0; cyc < 100; cyc++) {    /* 2 s closed */
        for (int ms = 0; ms < 20; ms++) {
            uint8_t ev = input_sample(&in, ms < 8);
            presses  += ev == INPUT_EV_PRESS;
            releases += ev == INPUT_EV_RELEASE || ev == INPUT_EV_RELEASE_AFTER_HOLD;
            holds    += ev == INPUT_EV_HOLD;
        }
    }
    CHECK(presses == 1 && releases == 0 && holds == 1);
    int at = -1;
    CHECK(feed(&in, 0, 60, &at) == INPUT_EV_RELEASE_AFTER_HOLD);
    CHECK(at <= INPUT_RELEASE_MS);           /* gap 12 ms + 25 ms from the last low */
}

int main(void) {
    test_press_seen_within_3_ms();
    test_spike_ignored();
    test_short_press_release();
    test_hold_and_release_after_hold();
    test_bounce_on_release_is_one_release();
    test_50hz_pulse_train_is_one_press();
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    printf("input_core: all tests passed\n");
    return EXIT_SUCCESS;
}
