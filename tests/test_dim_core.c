/* PC tests for the hardware-free dimming logic: make test */
#include "dim_core.h"
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

static void test_fade_up_takes_two_seconds(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 1000);
    int n = 0;
    while (dim_fader_tick(&f) != 1000 && n < 10000) {
        n++;
    }
    CHECK(n + 1 == 200);               /* 200 half cycles = 2 s at 50 Hz */
}

static void test_fade_lands_exactly_on_target(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 12);          /* not a multiple of the step */
    for (int i = 0; i < 10; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 12);
    dim_fader_set(&f, 1, 3);
    for (int i = 0; i < 10; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 3);
}

static void test_off_fades_to_zero_and_keeps_nothing(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 500);
    for (int i = 0; i < 200; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 500);
    dim_fader_set(&f, 0, 500);
    CHECK(f.target == 0);
    for (int i = 0; i < 99; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 5);
    CHECK(dim_fader_tick(&f) == 0);
    CHECK(dim_fader_tick(&f) == 0);
}

static void test_level_clamped(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 5000);
    CHECK(f.target == 1000);
}

static void test_compare(void) {
    CHECK(dim_compare(500, 0) == 0);
    CHECK(dim_compare(500, 1000) == 500);
    CHECK(dim_compare(500, 10) == 5);     /* minimum level: 100 us */
    CHECK(dim_compare(500, 500) == 250);
    CHECK(dim_compare(417, 1000) == 417); /* 60 Hz */
    CHECK(dim_compare(500, 2000) == 500); /* clamped */
    CHECK(dim_reload(500) > 500 && dim_reload(500) < 550);
}

static void test_zc_locks_after_regular_edges(void) {
    zc_tracker_t z;
    zc_tracker_init(&z);
    uint16_t t = 60000;                   /* wraps during the test */
    CHECK(zc_tracker_edge(&z, t) == 0);   /* first edge: nothing to measure */
    for (int i = 1; i < ZC_LOCK_EDGES; i++) {
        t = (uint16_t)(t + 500);
        CHECK(zc_tracker_edge(&z, t) == 1);
        CHECK(!zc_locked(&z));
    }
    t = (uint16_t)(t + 500);
    CHECK(zc_tracker_edge(&z, t) == 1);
    CHECK(zc_locked(&z));
    CHECK(z.half_period == 500);
}

static void test_zc_ignores_bounce(void) {
    zc_tracker_t z;
    zc_tracker_init(&z);
    uint16_t t = 0;
    zc_tracker_edge(&z, t);
    for (int i = 0; i < 10; i++) {
        t = (uint16_t)(t + 500);
        CHECK(zc_tracker_edge(&z, t) == 1);
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + 3)) == 0);    /* 60 us later */
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + 100)) == 0);  /* 2 ms later */
    }
    CHECK(zc_locked(&z));
    CHECK(z.half_period == 500);          /* bounces did not disturb timing */
}

static void test_zc_gap_drops_lock(void) {
    zc_tracker_t z;
    zc_tracker_init(&z);
    uint16_t t = 0;
    zc_tracker_edge(&z, t);
    for (int i = 0; i < 10; i++) {
        t = (uint16_t)(t + 500);
        zc_tracker_edge(&z, t);
    }
    CHECK(zc_locked(&z));
    CHECK(zc_tracker_check(&z, (uint16_t)(t + 600)) == 0);   /* a little late: fine */
    CHECK(zc_locked(&z));
    CHECK(zc_tracker_check(&z, (uint16_t)(t + 1000)) == 1);  /* 20 ms: mains gone */
    CHECK(!zc_locked(&z));
    CHECK(zc_tracker_check(&z, (uint16_t)(t + 2000)) == 0);  /* reported once */
    /* Edges come back after the gap: relock from scratch. */
    t = (uint16_t)(t + 3000);
    CHECK(zc_tracker_edge(&z, t) == 0);
    for (int i = 0; i < ZC_LOCK_EDGES; i++) {
        t = (uint16_t)(t + 500);
        zc_tracker_edge(&z, t);
    }
    CHECK(zc_locked(&z));
}

static void test_zc_follows_frequency_drift(void) {
    zc_tracker_t z;
    zc_tracker_init(&z);
    uint16_t t = 0;
    zc_tracker_edge(&z, t);
    for (int i = 0; i < 20; i++) {
        t = (uint16_t)(t + 500);
        zc_tracker_edge(&z, t);
    }
    for (int i = 0; i < 40; i++) {
        t = (uint16_t)(t + 505);          /* 49.5 Hz */
        zc_tracker_edge(&z, t);
    }
    CHECK(z.half_period >= 504 && z.half_period <= 506);
    t = (uint16_t)(t + 530);              /* one late edge */
    zc_tracker_edge(&z, t);
    CHECK(z.half_period < 512);
}

int main(void) {
    test_fade_up_takes_two_seconds();
    test_fade_lands_exactly_on_target();
    test_off_fades_to_zero_and_keeps_nothing();
    test_level_clamped();
    test_compare();
    test_zc_locks_after_regular_edges();
    test_zc_ignores_bounce();
    test_zc_gap_drops_lock();
    test_zc_follows_frequency_drift();
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    printf("dim_core: all tests passed\n");
    return EXIT_SUCCESS;
}
