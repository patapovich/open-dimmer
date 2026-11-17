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
    CHECK(dim_compare(500, DIM_DUTY_MAX) == 5000);     /* 2 us ticks: whole half cycle */
    CHECK(dim_compare(500, 5000) == 2500);
    CHECK(dim_compare(500, 100) == 50);                /* 1 %: 100 us */
    CHECK(dim_compare(417, DIM_DUTY_MAX) == 4170);     /* 60 Hz */
    CHECK(dim_compare(500, 20000) == 5000);            /* clamped */
    CHECK(dim_reload(500) > 5000 && dim_reload(500) < 5500);
    CHECK(dim_reload(625) > 6250);                     /* still fits 16 bits */
}

static void test_curves(void) {
    for (uint8_t c = 0; c < 4; c++) {
        CHECK(dim_curve(c, 0) == 0);
        CHECK(dim_curve(c, 1000) == DIM_DUTY_MAX);
        CHECK(dim_curve(c, 10) >= 1);                  /* minimum level is never dark */
        uint16_t prev = 0;
        for (uint16_t v = 1; v <= 1000; v++) {         /* monotonic, no jumps back */
            uint16_t d = dim_curve(c, v);
            CHECK(d >= prev);
            prev = d;
        }
    }
    CHECK(dim_curve(0, 500) == 5000);                  /* linear = stock */
    CHECK(dim_curve(0, 10) == 100);
    CHECK(dim_curve(1, 500) < 2000);                   /* led: half level ~18 % time */
    CHECK(dim_curve(1, 100) < 150);
    CHECK(dim_curve(3, 500) > 3000);                   /* incandescent: more time, sine power */
    CHECK(dim_curve(99, 500) == dim_curve(0, 500));    /* unknown curve = linear */
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


static void test_hold_follows_without_lag_and_freezes_on_release(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 500);
    for (int i = 0; i < 200; i++) {
        dim_fader_tick(&f);
    }
    dim_fader_hold_start(&f, +1, DIM_HOLD_RATE, 100, 900);
    for (int i = 0; i < 50; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 500 + 50 * DIM_HOLD_RATE);
    CHECK(f.target == f.current);            /* nothing left to catch up */
    uint16_t at_release = f.current;
    CHECK(dim_fader_hold_stop(&f) == at_release);
    for (int i = 0; i < 100; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == at_release);          /* no run-on after release */
    CHECK(f.step == DIM_STEP_DEFAULT);       /* normal speed right away */
}

static void test_hold_stops_at_bounds(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 800);
    for (int i = 0; i < 200; i++) {
        dim_fader_tick(&f);
    }
    dim_fader_hold_start(&f, +1, DIM_HOLD_RATE, 100, 900);
    for (int i = 0; i < 1000; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 900);
    dim_fader_hold_start(&f, -1, DIM_HOLD_RATE, 100, 900);
    for (int i = 0; i < 1000; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 100);                 /* min, never off */
}

static void test_hold_up_from_dark_starts_at_min(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 600);
    dim_fader_tick(&f);                      /* fading in: current 5 */
    dim_fader_hold_start(&f, +1, DIM_HOLD_RATE, 200, 1000);
    CHECK(dim_fader_tick(&f) == 200 + DIM_HOLD_RATE);
}

static void test_remote_write_during_hold(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 500);
    for (int i = 0; i < 200; i++) {
        dim_fader_tick(&f);
    }
    dim_fader_hold_start(&f, -1, DIM_HOLD_RATE, 100, 1000);
    dim_fader_set(&f, 1, 900);               /* a level from the network: finger wins */
    dim_fader_tick(&f);
    CHECK(f.current == 500 - DIM_HOLD_RATE);
    dim_fader_set(&f, 0, 900);               /* off wins */
    CHECK(f.hold_dir == 0 && f.target == 0);
}

static zc_tracker_t locked_at_500(uint16_t *t) {
    zc_tracker_t z;
    zc_tracker_init(&z);
    *t = 0;
    zc_tracker_edge(&z, *t);
    for (int i = 0; i < 10; i++) {
        *t = (uint16_t)(*t + 500);
        zc_tracker_edge(&z, *t);
    }
    return z;
}

static void test_zc_glitch_mid_cycle_is_ignored_when_locked(void) {
    uint16_t     t;
    zc_tracker_t z = locked_at_500(&t);
    CHECK(zc_locked(&z));
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 400)) == 0);   /* glitch at 8 ms */
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 500)) == 1);   /* the real one */
    CHECK(zc_locked(&z));
    CHECK(z.half_period == 500);
}

static void test_zc_edge_within_window_is_followed(void) {
    uint16_t     t;
    zc_tracker_t z = locked_at_500(&t);
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 520)) == 1);   /* 4 % late: fine */
    CHECK(zc_locked(&z));
}

static void test_zc_late_edge_relocks_without_output(void) {
    uint16_t     t;
    zc_tracker_t z = locked_at_500(&t);
    t = (uint16_t)(t + 1000);                                /* one edge missed */
    CHECK(zc_tracker_edge(&z, t) == 0);
    CHECK(!zc_locked(&z));
    for (int i = 0; i < ZC_LOCK_EDGES; i++) {
        t = (uint16_t)(t + 500);
        zc_tracker_edge(&z, t);
    }
    CHECK(zc_locked(&z));
    CHECK(z.half_period == 500);
}

static void test_off_during_hold_then_release_stays_off(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 800);
    for (int i = 0; i < 200; i++) {
        dim_fader_tick(&f);
    }
    dim_fader_hold_start(&f, -1, DIM_HOLD_RATE, 100, 1000);
    dim_fader_tick(&f);
    dim_fader_set(&f, 0, 800);               /* off from the network */
    dim_fader_tick(&f);                      /* fading out */
    dim_fader_hold_stop(&f);                 /* finger released mid fade */
    CHECK(f.target == 0);
    for (int i = 0; i < 300; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 0);
}

static void test_output_linear_is_unchanged(void) {
    for (uint16_t v = 10; v <= 1000; v += 7) {
        CHECK(dim_output(0, v, 10, 1000) == v * 10);          /* level/1000, like the stock fw */
        CHECK(dim_output(0, v, 200, 1000) == (v < 200 ? 2000 : v * 10));
        CHECK(dim_output(9, v, 200, 900) == dim_output(0, v, 200, 900));   /* unknown = linear */
    }
    CHECK(dim_output(0, 0, 200, 1000) == 0);
}

static void test_output_min_max_are_physical_for_every_curve(void) {
    for (uint8_t c = 0; c < 4; c++) {
        CHECK(dim_output(c, 287, 287, 900) == 2870);    /* min: the same duty whatever the curve */
        CHECK(dim_output(c, 900, 287, 900) == 9000);    /* max: likewise */
        CHECK(dim_output(c, 100, 287, 900) == 2870);    /* below min (fading in): min */
        CHECK(dim_output(c, 0, 287, 900) == 0);
        uint16_t prev = 0;
        for (uint16_t v = 287; v <= 900; v++) {
            uint16_t d = dim_output(c, v, 287, 900);
            CHECK(d >= prev);
            prev = d;
        }
    }
    /* led: the middle of the range sits low, as the curve shapes it */
    CHECK(dim_output(1, 593, 287, 900) < dim_output(0, 593, 287, 900));
    CHECK(dim_output(1, 593, 50, 1000) > 2700 && dim_output(1, 593, 50, 1000) < 3100);   /* ~2890 */
}

static void test_fader_floor(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set_floor(&f, 287);
    dim_fader_set(&f, 1, 500);
    CHECK(dim_fader_tick(&f) == 292);       /* on: starts at the floor, then fades */
    for (int i = 0; i < 100; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 500);
    dim_fader_set(&f, 0, 500);
    int n = 0;
    while (f.current && n < 1000) {
        uint16_t prev = f.current;
        uint16_t now  = dim_fader_tick(&f);
        CHECK(now == 0 || now >= 287);      /* never lingers below the floor */
        CHECK(now < prev);
        n++;
    }
    CHECK(n <= (500 - 287) / 5 + 2);        /* off ends at the floor, no 0.6 s tail */
    /* hold from dark still starts at the hold lo */
    dim_fader_set(&f, 1, 287);
    dim_fader_tick(&f);
    CHECK(f.current == 287);
    /* floor 0 (the default) = the old behaviour */
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 20);
    CHECK(dim_fader_tick(&f) == 5);
}

int main(void) {
    test_fade_up_takes_two_seconds();
    test_fade_lands_exactly_on_target();
    test_off_fades_to_zero_and_keeps_nothing();
    test_level_clamped();
    test_compare();
    test_curves();
    test_output_linear_is_unchanged();
    test_output_min_max_are_physical_for_every_curve();
    test_fader_floor();
    test_zc_locks_after_regular_edges();
    test_zc_ignores_bounce();
    test_zc_gap_drops_lock();
    test_zc_follows_frequency_drift();
    test_zc_glitch_mid_cycle_is_ignored_when_locked();
    test_zc_edge_within_window_is_followed();
    test_zc_late_edge_relocks_without_output();
    test_hold_follows_without_lag_and_freezes_on_release();
    test_hold_stops_at_bounds();
    test_hold_up_from_dark_starts_at_min();
    test_remote_write_during_hold();
    test_off_during_hold_then_release_stays_off();
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    printf("dim_core: all tests passed\n");
    return EXIT_SUCCESS;
}
