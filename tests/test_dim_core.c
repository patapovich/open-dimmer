/* PC tests for the hardware-free dimming logic: make test */
#include "dim_core.h"
#include "curves.h"

/* the default profile in whole steps (1/64 steps / 64) */
#define ON_STEP   ((640000 + DIM_FADE_ON_MS / 2) / DIM_FADE_ON_MS / 64)
#define SOFT_SPAN (DIM_SOFT_END_PCT * 10)
#define HOLD_SPAN (DIM_HOLD_END_PCT * 10)
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
    for (uint8_t c = 0; c < CURVE_COUNT; c++) {
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

/* dim_curve() interpolates with unsigned maths (no signed division code in
 * the image): that needs tables that never fall, which tools/gen_curves.py
 * enforces. */
static void test_curve_tables_never_fall(void) {
    for (uint8_t c = 0; c < CURVE_COUNT - 1; c++) {    /* linear has no table */
        CHECK(k_curves[c][0] == 0);
        CHECK(k_curves[c][CURVE_POINTS - 1] == CURVE_OUT_MAX);
        for (uint8_t i = 0; i + 1 < CURVE_POINTS; i++) {
            CHECK(k_curves[c][i + 1] >= k_curves[c][i]);
        }
    }
    CHECK(CURVE_OUT_MAX == DIM_DUTY_MAX);
}

/* The interpolation as it was with signed maths (up to 2.1.11). */
static uint16_t curve_signed(uint8_t curve, uint16_t value) {
    if (value == 0) {
        return 0;
    }
    if (value >= DIM_VALUE_MAX) {
        return DIM_DUTY_MAX;
    }
    if (curve == CURVE_LINEAR || curve >= CURVE_COUNT) {
        return (uint16_t)(value * 10u);                /* linear: computed, as dim_output does */
    }
    const uint16_t *k    = k_curves[curve - 1];
    uint32_t        pos  = (uint32_t)value * (CURVE_POINTS - 1);
    uint32_t        seg  = pos / DIM_VALUE_MAX;
    uint32_t        frac = pos % DIM_VALUE_MAX;
    int32_t         a = k[seg], b = k[seg + 1];
    int32_t         v = a + ((b - a) * (int32_t)frac + DIM_VALUE_MAX / 2) / DIM_VALUE_MAX;
    return (uint16_t)(v < 1 ? 1 : v);
}

static void test_curve_unsigned_maths_gives_the_same_duty(void) {
    int differ = 0;
    for (unsigned c = 0; c < 256; c++) {                 /* unknown curves too (= linear) */
        for (unsigned v = 0; v <= 1100; v++) {
            differ += dim_curve((uint8_t)c, (uint16_t)v) != curve_signed((uint8_t)c, (uint16_t)v);
        }
        differ += dim_curve((uint8_t)c, 65535) != curve_signed((uint8_t)c, 65535);
    }
    CHECK(differ == 0);
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
        CHECK(zc_tracker_edge(&z, t) == 1);
    }
    CHECK(z.half_period >= 504 && z.half_period <= 506);
    t = (uint16_t)(t + 520);              /* one edge 3 % late: followed, but */
    CHECK(zc_tracker_edge(&z, t) == 1);
    t = (uint16_t)(t + 505);
    CHECK(zc_tracker_edge(&z, t) == 1);
    CHECK(z.half_period < 510);           /* it moves the timing by a quarter */
    CHECK(zc_locked(&z));
}

/* ---- zero cross: the limits as numbers, so a changed constant fails ---- */

_Static_assert(ZC_HALF_MIN_TICKS >= 357, "edges closer than 7.14 ms (70 Hz) are never a zero cross");
_Static_assert(ZC_HALF_MAX_TICKS <= 625, "a half cycle longer than 12.5 ms (40 Hz) is a gap");
_Static_assert(ZC_LOCK_EDGES >= 4, "at least 4 good half cycles in a row before any output");
_Static_assert(ZC_WINDOW_DIV >= 24, "an edge is taken at most +-1/24 from where the learned half cycles put it");
_Static_assert(ZC_HALF_MIN_TICKS / ZC_WINDOW_DIV >= 4, "the window is wider than the detector jitter");
_Static_assert((ZC_HALF_MAX_TICKS + ZC_HALF_MAX_TICKS / 16) * 10 <= 0xFFFF, "TIM1 reload fits 16 bits");

static zc_tracker_t locked_at(uint16_t hp, uint16_t *t) {
    zc_tracker_t z;
    zc_tracker_init(&z);
    *t = 1000;
    zc_tracker_edge(&z, *t);
    for (int i = 0; i < 10; i++) {
        *t = (uint16_t)(*t + hp);
        zc_tracker_edge(&z, *t);
    }
    return z;
}

/* Locked onto half cycles alternating first, second, ...; the next is first. */
static zc_tracker_t locked_alternating(uint16_t first, uint16_t second, uint16_t *t) {
    zc_tracker_t z;
    zc_tracker_init(&z);
    *t = 3000;
    zc_tracker_edge(&z, *t);
    for (int i = 0; i < 10; i++) {
        *t = (uint16_t)(*t + ((i & 1) ? second : first));
        zc_tracker_edge(&z, *t);
    }
    return z;
}

static void test_zc_lock_takes_four_good_half_cycles(void) {
    zc_tracker_t z;
    zc_tracker_init(&z);
    uint16_t t = 0;
    CHECK(zc_tracker_edge(&z, t) == 0);
    for (int i = 1; i <= 3; i++) {
        t = (uint16_t)(t + 500);
        CHECK(zc_tracker_edge(&z, t) == 1);
        CHECK(!zc_locked(&z));                    /* 3 good half cycles: not yet */
    }
    t = (uint16_t)(t + 500);
    CHECK(zc_tracker_edge(&z, t) == 1);
    CHECK(zc_locked(&z));                         /* the 4th */
}

static void test_zc_window_edges_when_locked(void) {
    /* 50 Hz: +-20 ticks (0.4 ms) */
    const struct { uint16_t hp, dt; uint8_t acted, locked; } k[] = {
        { 500, 479, 0, 1 }, { 500, 480, 1, 1 }, { 500, 520, 1, 1 }, { 500, 521, 0, 0 },
        { 417, 399, 0, 1 }, { 417, 400, 1, 1 }, { 417, 434, 1, 1 }, { 417, 435, 0, 0 },   /* 60 Hz: 17 */
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        uint16_t     t;
        zc_tracker_t z = locked_at(k[i].hp, &t);
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + k[i].dt)) == k[i].acted);
        CHECK(zc_locked(&z) == k[i].locked);
    }
}

static void test_zc_window_from_the_first_good_half_cycle(void) {
    /* second edge: within 1/12 (40 ticks) of the first half cycle */
    const struct { uint16_t dt; uint8_t acted; uint8_t good; } k2[] = {
        { 459, 0, 1 }, { 460, 1, 2 }, { 540, 1, 2 }, { 541, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof k2 / sizeof k2[0]; i++) {
        zc_tracker_t z;
        zc_tracker_init(&z);
        zc_tracker_edge(&z, 100);
        CHECK(zc_tracker_edge(&z, 600) == 1);
        CHECK(zc_tracker_edge(&z, (uint16_t)(600 + k2[i].dt)) == k2[i].acted);
        CHECK(z.good == k2[i].good);
    }
    /* third edge: within 1/24 (20) of the first one, its own kind */
    const struct { uint16_t dt; uint8_t acted; uint8_t good; } k3[] = {
        { 479, 0, 2 }, { 480, 1, 3 }, { 520, 1, 3 }, { 521, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof k3 / sizeof k3[0]; i++) {
        zc_tracker_t z;
        zc_tracker_init(&z);
        zc_tracker_edge(&z, 100);
        zc_tracker_edge(&z, 600);
        CHECK(zc_tracker_edge(&z, 1100) == 1);
        CHECK(zc_tracker_edge(&z, (uint16_t)(1100 + k3[i].dt)) == k3[i].acted);
        CHECK(z.good == k3[i].good);
    }
}

static void test_zc_noise_on_the_lock_completing_edge(void) {
    /* a pulse 7.14..9.6 ms after the 4th edge does not complete the lock (it
     * did: the gate turned on there at up to ~250 V); the real edge does */
    for (uint16_t off = 357; off + 2 < 480; off++) {          /* the pulse ends before the window */
        zc_tracker_t z;
        zc_tracker_init(&z);
        uint16_t t = 0;
        zc_tracker_edge(&z, t);
        for (int i = 0; i < 3; i++) {
            t = (uint16_t)(t + 500);
            zc_tracker_edge(&z, t);
        }
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + off)) == 0);
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + off + 2)) == 0);
        CHECK(!zc_locked(&z));
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + 500)) == 1);
        CHECK(zc_locked(&z) && z.half_period == 500);
    }
}

static void test_zc_noise_train_near_the_edge_does_not_capture_a_lock(void) {
    /* a pulse every half cycle 9.3 ms in (0.7 ms before the real edge): inside
     * the old +-8 % window it took over the timing (~70 V turn-ons) */
    uint16_t     t;
    zc_tracker_t z = locked_at(500, &t);
    int          noise = 0, real = 0;
    for (int i = 0; i < 200; i++) {
        noise += zc_tracker_edge(&z, (uint16_t)(t + 465));
        noise += zc_tracker_edge(&z, (uint16_t)(t + 467));
        t = (uint16_t)(t + 500);
        real += zc_tracker_edge(&z, t);
    }
    CHECK(noise == 0 && real == 200);
    CHECK(zc_locked(&z) && z.half_period == 500);
}

static void test_zc_noise_inside_the_window_costs_one_half_cycle(void) {
    /* 0.2 ms before the real edge: cannot be told from it (~20 V). The real
     * edge after it is bounce, the next one is on time again. */
    uint16_t     t;
    zc_tracker_t z = locked_at(500, &t);
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 490)) == 1);
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 500)) == 0);
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 1000)) == 1);
    CHECK(zc_locked(&z));
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 1500)) == 1);
    CHECK(zc_locked(&z) && z.half_period == 500);   /* neither half cycle at the pulse learned */
    CHECK(z.hp[0] == 500 && z.hp[1] == 500);
}

static void test_zc_noise_inside_the_window_does_not_move_the_window(void) {
    /* In-window pulses (an edge, its return, then the real edge) on the same
     * kind of half cycle for several periods: hp[] stays on the mains, so a
     * later pulse 1.2..1.6 windows early is still ignored. When the noise
     * moved the two kinds apart, it was taken up to 31 ticks (0.62 ms) early.
     * Asymmetric detectors: the pulses on the longer kind (on the shorter one
     * the next half cycle, longer by the pulse, can pass the 8 % limit). */
    const struct { uint16_t a, b; } hz[] = { { 500, 500 }, { 417, 417 }, { 514, 486 }, { 431, 403 } };
    for (unsigned h = 0; h < sizeof hz / sizeof hz[0]; h++) {
        uint16_t w = (uint16_t)(hz[h].b / ZC_WINDOW_DIV);   /* from the last half cycle */
        for (uint16_t early = ZC_BOUNCE_TICKS + 1; early <= w; early++) {
            for (int periods = 1; periods <= 6; periods++) {
                uint16_t     t;
                zc_tracker_t z  = locked_alternating(hz[h].a, hz[h].b, &t);   /* next: a */
                uint16_t     h0 = z.hp[0], h1 = z.hp[1];
                for (int p = 0; p < periods; p++) {
                    CHECK(zc_tracker_edge(&z, (uint16_t)(t + hz[h].a - early)) == 1);   /* taken */
                    CHECK(zc_tracker_edge(&z, (uint16_t)(t + hz[h].a - early + 2)) == 0);
                    t = (uint16_t)(t + hz[h].a);
                    CHECK(zc_tracker_edge(&z, t) == 0);                              /* the real edge */
                    t = (uint16_t)(t + hz[h].b);
                    CHECK(zc_tracker_edge(&z, t) == 1);
                    CHECK(zc_locked(&z));
                }
                CHECK(z.hp[0] == h0 && z.hp[1] == h1);
                for (uint16_t far = (uint16_t)(w + w / 5); far <= w + (w * 3) / 5; far++) {
                    zc_tracker_t y = z;
                    CHECK(zc_tracker_edge(&y, (uint16_t)(t + hz[h].a - far)) == 0);
                    CHECK(zc_locked(&y));
                }
            }
        }
    }
}

static void test_zc_late_isr_moves_the_window_by_a_quarter_of_its_delay(void) {
    /* An ISR held back by d (inside the window): its half cycle is d long, the
     * next d short, so the kinds move d/4 apart. In the periods after it a
     * pulse is taken at most window + d/4 (rounded) before the real edge. */
    for (uint16_t d = 1; d <= 500 / ZC_WINDOW_DIV; d++) {
        uint16_t q   = (uint16_t)((d + 2) / 4);
        uint16_t lim = (uint16_t)((500 + q) / ZC_WINDOW_DIV + q);   /* the window of the longer kind */
        for (int after = 0; after < 8; after++) {
            for (uint16_t early = (uint16_t)(lim - 3); early <= lim + 3; early++) {
                uint16_t     t;
                zc_tracker_t z = locked_at(500, &t);
                CHECK(zc_tracker_edge(&z, (uint16_t)(t + 500 + d)) == 1);   /* late ISR */
                t = (uint16_t)(t + 500);
                for (int i = 0; i <= after; i++) {
                    t = (uint16_t)(t + 500);
                    CHECK(zc_tracker_edge(&z, t) == 1);
                }
                int taken = zc_tracker_edge(&z, (uint16_t)(t + 500 - early));
                CHECK(zc_locked(&z));
                CHECK(!taken || early <= lim);
            }
        }
    }
}

static void test_zc_extra_edge_limits(void) {
    /* An edge up to ZC_BOUNCE_TICKS after the one taken is its bounce; from
     * there to ZC_SUSPECT_TICKS it may be the real edge after a noise pulse:
     * acquiring, the count starts again from it. Later ones are noise after
     * the zero cross. */
    _Static_assert(ZC_BOUNCE_TICKS == 5, "bounce: up to 0.1 ms after the edge");
    _Static_assert(ZC_SUSPECT_TICKS == 52, "suspect: up to 1/12 of a 40 Hz half cycle");
    const struct { uint16_t extra; uint8_t restarts; } k[] = {
        { 1, 0 }, { 5, 0 }, { 6, 1 }, { 30, 1 }, { 52, 1 }, { 53, 0 }, { 356, 0 },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        zc_tracker_t z;
        zc_tracker_init(&z);
        uint16_t t = 100;
        zc_tracker_edge(&z, t);
        for (int e = 1; e <= 3; e++) {
            t = (uint16_t)(t + 500);
            CHECK(zc_tracker_edge(&z, t) == 1);
        }
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + k[i].extra)) == 0);
        CHECK(z.good == (k[i].restarts ? 0 : 3));
        CHECK(z.last == (uint16_t)(t + k[i].extra) || !k[i].restarts);
        /* from the restart, 4 good half cycles again; else the next completes the lock */
        uint16_t from = k[i].restarts ? (uint16_t)(t + k[i].extra) : t;
        for (int e = 1; e <= (k[i].restarts ? 4 : 1); e++) {
            CHECK(!zc_locked(&z));
            CHECK(zc_tracker_edge(&z, (uint16_t)(from + 500 * e)) == 1);
        }
        CHECK(zc_locked(&z));
    }
}

static void test_zc_pulse_just_before_the_lock_completing_edge_restarts_the_count(void) {
    /* A pulse 0.2 ms before a real edge while acquiring: the real edge right
     * after it starts the count again, from itself, so a pulse cannot leave a
     * wrong first sample behind for the lock. On the 4th half cycle it
     * completes the lock (it cannot be told from the real edge, and it is
     * inside the window); the real edge after it then undoes what it taught. */
    for (int at = 1; at <= 4; at++) {
        zc_tracker_t z;
        zc_tracker_init(&z);
        uint16_t t = 0;
        zc_tracker_edge(&z, t);
        for (int e = 1; e < at; e++) {
            t = (uint16_t)(t + 500);
            CHECK(zc_tracker_edge(&z, t) == 1);
        }
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + 490)) == 1);
        CHECK(zc_locked(&z) == (at == 4));
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + 492)) == 0);
        t = (uint16_t)(t + 500);
        CHECK(zc_tracker_edge(&z, t) == 0);
        if (at == 4) {
            CHECK(zc_locked(&z) && z.hp[0] == 500 && z.hp[1] == 500);
            continue;
        }
        CHECK(z.good == 0 && z.last == t);
        for (int e = 1; e <= 4; e++) {
            CHECK(!zc_locked(&z));
            t = (uint16_t)(t + 500);
            CHECK(zc_tracker_edge(&z, t) == 1);
        }
        CHECK(zc_locked(&z) && z.hp[0] == 500 && z.hp[1] == 500);
    }
}

static void test_zc_bounce_does_not_stop_the_learning(void) {
    /* an edge's own bounce (up to 0.1 ms after it) is not noise: the drift is followed */
    zc_tracker_t z;
    zc_tracker_init(&z);
    uint16_t t = 0;
    zc_tracker_edge(&z, t);
    for (int i = 0; i < 60; i++) {
        t = (uint16_t)(t + (i < 20 ? 500 : 505));
        CHECK(zc_tracker_edge(&z, t) == 1);
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + ZC_BOUNCE_TICKS)) == 0);
    }
    CHECK(zc_locked(&z) && z.hp[0] >= 504 && z.hp[1] >= 504);
}

static void test_zc_extra_edge_after_every_zero_cross_never_locks(void) {
    /* a detector that chatters 0.12..1 ms after every edge: never locked (dark) */
    zc_tracker_t z;
    zc_tracker_init(&z);
    uint16_t t = 0;
    zc_tracker_edge(&z, t);
    for (int i = 0; i < 400; i++) {
        t = (uint16_t)(t + 500);
        zc_tracker_edge(&z, t);
        zc_tracker_edge(&z, (uint16_t)(t + ZC_BOUNCE_TICKS + 1));
        CHECK(!zc_locked(&z));
    }
}

static void test_zc_half_period_never_below_the_minimum(void) {
    /* locked at 67.6 Hz, an edge at 356 ticks fits the window (370 +- 15)
     * but not the 70 Hz limit; edges at the limit keep it there */
    uint16_t     t;
    zc_tracker_t z = locked_at(370, &t);
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 356)) == 0);
    CHECK(zc_locked(&z));
    for (int i = 0; i < 200; i++) {
        t = (uint16_t)(t + 357);
        zc_tracker_edge(&z, t);
        CHECK(z.half_period >= ZC_HALF_MIN_TICKS && z.hp[0] >= 357 && z.hp[1] >= 357);
    }
    CHECK(zc_locked(&z));
    zc_tracker_init(&z);                          /* and while acquiring */
    zc_tracker_edge(&z, 0);
    CHECK(zc_tracker_edge(&z, 356) == 0);
    CHECK(zc_tracker_edge(&z, 357) == 1);
}

static void test_zc_late_edge_inside_the_gap_limit_relocks(void) {
    /* 12 % late, short of the 625-tick gap: the late branch, not the gap one */
    uint16_t     t;
    zc_tracker_t z = locked_at(500, &t);
    t = (uint16_t)(t + 560);
    CHECK(zc_tracker_edge(&z, t) == 0);
    CHECK(!zc_locked(&z) && z.good == 0);
    for (int i = 1; i <= 4; i++) {
        t = (uint16_t)(t + 500);
        CHECK(zc_tracker_edge(&z, t) == 1);
        CHECK(zc_locked(&z) == (i == 4));
    }
}

static void test_zc_asymmetric_detector(void) {
    /* PA4 high and low of different length: the two kinds are timed
     * separately, and the half period is that of the half cycle starting now */
    const uint16_t pair[][2] = { { 486, 514 }, { 403, 431 }, { 500, 504 } };
    for (unsigned p = 0; p < sizeof pair / sizeof pair[0]; p++) {
        zc_tracker_t z;
        zc_tracker_init(&z);
        uint16_t t = 7;
        zc_tracker_edge(&z, t);
        int acted = 0, wrong = 0;
        for (int i = 0; i < 400; i++) {
            t = (uint16_t)(t + pair[p][i & 1]);
            acted += zc_tracker_edge(&z, t);
            if (i >= 3 && z.half_period != pair[p][(i + 1) & 1]) {
                wrong++;
            }
            if (i >= 3 && !zc_locked(&z)) {
                wrong++;
            }
        }
        CHECK(acted == 400 && wrong == 0);
    }
    /* 9.2 / 10.8 ms (16 % apart): not a detector, or noise taking every
     * other edge; never locks */
    zc_tracker_t z;
    zc_tracker_init(&z);
    uint16_t t = 0;
    zc_tracker_edge(&z, t);
    for (int i = 0; i < 400; i++) {
        t = (uint16_t)(t + ((i & 1) ? 540 : 460));
        zc_tracker_edge(&z, t);
        CHECK(!zc_locked(&z));
    }
}

static void test_zc_kinds_at_the_8_percent_limit(void) {
    /* 9.6 / 10.4 ms: the two kinds 1/12 apart. An edge that fits its own
     * kind but takes them further apart does not count: earlier than 1/12
     * before the last half cycle is ignored, later than 1/12 after it drops
     * the lock. */
    uint16_t     t;
    zc_tracker_t z = locked_alternating(480, 520, &t);
    CHECK(zc_locked(&z) && z.half_period == 480);
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 478)) == 1);       /* 520 - 2 * 520/24 = 478 */
    z = locked_alternating(480, 520, &t);
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 477)) == 0);
    CHECK(zc_locked(&z));
    z = locked_alternating(520, 480, &t);
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 520)) == 1);       /* 480 + 2 * 480/24 = 520 */
    z = locked_alternating(520, 480, &t);
    CHECK(zc_tracker_edge(&z, (uint16_t)(t + 521)) == 0);
    CHECK(!zc_locked(&z));
}

static void test_output_max_clamps_for_every_curve(void) {
    /* max lowered below the level the fader still has */
    for (unsigned c = 0; c <= CURVE_COUNT; c++) {               /* and an unknown curve */
        CHECK(dim_output((uint8_t)c, 950, 100, 600) == 6000);
        CHECK(dim_output((uint8_t)c, 1000, 100, 600) == 6000);
        CHECK(dim_output((uint8_t)c, 601, 100, 600) == 6000);
    }
}

static void test_hold_time_zero_is_the_100_ms_floor(void) {
    dim_profile_t p0, p100, p101;
    dim_profile_set(&p0, 950, 950, 12, 0, 20);
    dim_profile_set(&p100, 950, 950, 12, 100, 20);
    dim_profile_set(&p101, 950, 950, 12, 101, 20);
    CHECK(p0.hold_q == 6400);                     /* the whole range in 100 ms, not at once */
    CHECK(p100.hold_q == 6400);
    CHECK(p101.hold_q < 6400);
}


static void test_hold_follows_without_lag_and_freezes_on_release(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 500);
    for (int i = 0; i < 200; i++) {
        dim_fader_tick(&f);
    }
    dim_fader_hold_start(&f, +1, 100, 900);
    for (int i = 0; i < 50; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current >= 500 + 50 * DIM_HOLD_RATE && f.current <= 500 + 50 * DIM_HOLD_RATE + 2);   /* 3300 ms: 3.03 per 10 ms */
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
    dim_fader_hold_start(&f, +1, 100, 900);
    for (int i = 0; i < 1000; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 900);
    dim_fader_hold_start(&f, -1, 100, 900);
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
    dim_fader_hold_start_from_dark(&f, 200, 1000);
    CHECK(dim_fader_tick(&f) == 200);        /* at the min, moving slowly */
}

static void test_hold_is_slow_just_above_the_min(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 60);
    for (int i = 0; i < 50; i++) {
        dim_fader_tick(&f);
    }
    dim_fader_hold_start_from_dark(&f, 60, 1000);
    int n50 = 0, n200 = 0, n = 0;
    uint16_t prev = 60;
    while (f.current < 60 + HOLD_SPAN + 100 && n < 2000) {
        uint16_t now = dim_fader_tick(&f);
        CHECK(now >= prev && now - prev <= DIM_HOLD_RATE + 1);
        prev = now;
        n++;
        if (now < 60 + 50) {
            n50 = n;
        }
        if (now < 60 + HOLD_SPAN) {
            n200 = n;
        }
    }
    CHECK(n50 > 3 * 50 / DIM_HOLD_RATE);              /* the dimmest 50 take > 3x longer */
    CHECK(n200 > 2 * HOLD_SPAN / DIM_HOLD_RATE);
    CHECK(n200 < 200);                                  /* still under 2 s */
    /* down towards the min: slow there too, and stops at it */
    dim_fader_hold_start(&f, -1, 60, 1000);
    for (int i = 0; i < 2000; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 60);
    /* profile: no slow-down = the plain rate */
    dim_profile_t p;
    dim_profile_default(&p);
    CHECK(p.hold_span == HOLD_SPAN && p.hold_bottom_q == DIM_HOLD_BOTTOM_Q && p.hold_q == 194);
    p.hold_span = 0;
    dim_fader_set_profile(&f, &p);
    dim_fader_hold_start(&f, +1, 60, 1000);
    CHECK(dim_fader_tick(&f) == 63);
}

static void test_remote_write_during_hold(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 500);
    for (int i = 0; i < 200; i++) {
        dim_fader_tick(&f);
    }
    dim_fader_hold_start(&f, -1, 100, 1000);
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
    dim_fader_hold_start(&f, -1, 100, 1000);
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
    for (uint8_t c = 0; c < CURVE_COUNT; c++) {
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
    dim_fader_set(&f, 1, 600);
    CHECK(dim_fader_tick(&f) == 287 + ON_STEP);   /* on: from the floor, quick */
    int n_on = 1;
    uint16_t prev = 287 + ON_STEP;
    while (f.current != 600 && n_on < 1000) {
        uint16_t now = dim_fader_tick(&f);
        CHECK(now > prev);
        prev = now;
        n_on++;
    }
    CHECK(f.current == 600);
    CHECK(n_on >= (600 - 287) * DIM_FADE_ON_MS / 10000 && n_on <= (600 - 287) * DIM_FADE_ON_MS / 10000 + 2);

    dim_fader_set(&f, 0, 600);              /* off */
    int at_floor = 0, n_off = 0, last_steps = 0;
    prev = 600;
    while (f.current && n_off < 1000) {
        uint16_t now = dim_fader_tick(&f);
        CHECK(now == 0 || now >= 287);      /* never lingers below the floor */
        CHECK(now <= prev);
        if (now == 287) {
            at_floor++;
        }
        if (now > 0 && now < 287 + SOFT_SPAN / 4 && now != 287) {
            last_steps++;                   /* the dim end is not rushed */
        }
        prev = now;
        n_off++;
    }
    CHECK(f.current == 0);
    CHECK(at_floor == DIM_OFF_HOLD_MS / 10); /* a moment at the floor (80 ms), then dark */
    CHECK(last_steps >= 10);                 /* the plain off rate would take ~3 */
    CHECK(n_off < 200);                     /* still under 2 s from 600 */

    /* a level change between lit levels above the ease span is the plain fade */
    dim_fader_set(&f, 1, 800);
    for (int k = 0; k < 400; k++) {
        dim_fader_tick(&f);
    }
    dim_fader_set(&f, 1, 600);
    CHECK(dim_fader_tick(&f) == 795);       /* lit to lit: the plain step */
    for (int k = 0; k < 400; k++) {
        dim_fader_tick(&f);
    }
    dim_fader_set(&f, 1, 300);              /* down near the floor, still lit: plain too */
    for (int k = 0; k < 59; k++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 305);
    dim_fader_set(&f, 1, 500);              /* back up while lit: the plain step, not the on step */
    CHECK(dim_fader_tick(&f) == 305 + DIM_STEP_DEFAULT);
    /* hold from dark still starts at the hold lo */
    dim_fader_set(&f, 1, 287);
    for (int k = 0; k < 400; k++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 287);
    /* floor 0 (the default) = the old behaviour */
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 20);
    CHECK(dim_fader_tick(&f) == 5);
}

static void test_hold_from_dark_and_cancel(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set_floor(&f, 60);
    dim_fader_set(&f, 1, 700);                /* on ... */
    dim_fader_hold_start_from_dark(&f, 60, 1000);   /* ... rising from the bottom */
    CHECK(dim_fader_tick(&f) == 60);          /* at the min first */
    for (int k = 0; k < 9; k++) {
        dim_fader_tick(&f);                   /* a 100 ms tap */
    }
    uint16_t tap = f.current;
    CHECK(tap >= 60 && tap < 60 + 10);
    dim_fader_hold_cancel(&f);
    dim_fader_set(&f, 1, 700);
    CHECK(dim_fader_tick(&f) == tap + ON_STEP);   /* quick on */
    int n = 0;
    while (f.current != 700 && n < 1000) {
        dim_fader_tick(&f);
        n++;
    }
    CHECK(f.current == 700);
    CHECK(n < (700 - 60) / ON_STEP + 2);
    /* a real hold from dark: rises from the floor, frozen at release */
    dim_fader_set(&f, 0, 700);
    while (f.current) {
        dim_fader_tick(&f);
    }
    dim_fader_set(&f, 1, 700);
    dim_fader_hold_start_from_dark(&f, 60, 1000);
    for (int k = 0; k < 40; k++) {
        dim_fader_tick(&f);
    }
    uint16_t held = f.current;
    CHECK(held > 60 && held < 60 + 40 * DIM_HOLD_RATE);   /* rising slowly from the bottom */
    CHECK(dim_fader_hold_stop(&f) == held);
    CHECK(dim_fader_tick(&f) == held);                    /* stays */
}

static void test_fade_profile(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_profile_t p;
    dim_profile_set(&p, 500, 2500, 0, 3300, 20);    /* on 20 per half cycle, off 4, no soft end */
    CHECK(p.on_q == 1280 && p.off_q == 256 && p.soft_span == 0 && p.hold_q == 194);
    p.off_hold_ms = 0;
    dim_fader_set_profile(&f, &p);
    dim_fader_set_floor(&f, 100);
    dim_fader_set(&f, 1, 500);
    CHECK(dim_fader_tick(&f) == 120);
    while (f.current != 500) {
        dim_fader_tick(&f);
    }
    dim_fader_set(&f, 0, 500);
    uint16_t prev = 500;
    int n = 0;
    while (f.current) {
        uint16_t now = dim_fader_tick(&f);
        CHECK(now == 0 || prev - now == 4 || now == 100);   /* plain steps of 4 */
        prev = now;
        n++;
    }
    CHECK(n == (500 - 100) / 4 + 1);                       /* at the floor once, then out */
    /* times: 0 = at once, slow times keep their fraction (1/64 steps) */
    dim_profile_set(&p, 0, 10000, 12, 10000, 20);
    CHECK(p.on_q == 64000 && p.off_q == 64 && p.hold_q == 64);
    dim_fader_init(&f);
    dim_fader_set_profile(&f, &p);
    dim_fader_set_floor(&f, 50);
    dim_fader_set(&f, 1, 1000);
    CHECK(dim_fader_tick(&f) == 1000);                     /* fade on 0 ms: at once */
    dim_profile_set(&p, 4000, 950, 12, 3300, 20);          /* 4 s: 2.5 per half cycle */
    dim_fader_set_profile(&f, &p);
    dim_fader_set(&f, 0, 1000);
    while (f.current) {
        dim_fader_tick(&f);
    }
    dim_fader_set(&f, 1, 1000);
    int k = 0;
    while (f.current != 1000 && k < 2000) {
        dim_fader_tick(&f);
        k++;
    }
    CHECK(k >= 378 && k <= 382);                           /* (1000 - 50) / 2.5 */
}

static void test_hold_up_from_a_lit_level_is_not_slowed(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set_floor(&f, 60);
    dim_fader_set(&f, 1, 130);                 /* lit, just above the min */
    for (int i = 0; i < 100; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 130);
    dim_fader_hold_start(&f, +1, 60, 1000);    /* up from there: full rate at once */
    CHECK(dim_fader_tick(&f) == 130 + DIM_HOLD_RATE);
    for (int i = 0; i < 20; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current == 130 + 21 * DIM_HOLD_RATE);
    dim_fader_hold_stop(&f);
    dim_fader_hold_start(&f, -1, 60, 1000);    /* down towards the min: slows there */
    int n = 0;
    while (f.current > 60 && n < 2000) {
        dim_fader_tick(&f);
        n++;
    }
    CHECK(n > (130 + 21 * DIM_HOLD_RATE - 60) / DIM_HOLD_RATE * 2);
}

/* Half cycles until the fader reaches target, ticking at half period hp. */
static int ticks_to(dim_fader_t *f, uint16_t target, uint16_t hp) {
    int n = 0;
    dim_fader_set_half_period(f, hp);
    while (f->current != target && n < 5000) {
        dim_fader_tick(f);
        n++;
    }
    return n;
}

static int ms_of(int ticks, uint16_t hp) {
    return (int)((long)ticks * hp * 20 / 1000);
}

static void test_times_in_ms_hold_on_50_and_60_hz(void) {
    const uint16_t hps[] = { 500, 417 };          /* 50 Hz, 60 Hz */
    int on_ms[2], off_ms[2], hold_ms[2], plain_ms[2], at_floor[2];
    for (int k = 0; k < 2; k++) {
        uint16_t hp = hps[k];
        dim_fader_t f;
        dim_fader_init(&f);
        dim_fader_set_floor(&f, 59);
        dim_fader_set(&f, 1, 1000);                     /* on: 941 of 1000 at 950 ms per range */
        on_ms[k] = ms_of(ticks_to(&f, 1000, hp), hp);
        dim_fader_set(&f, 1, 500);                      /* lit to lit: 500 at 5 per 10 ms = 1 s */
        plain_ms[k] = ms_of(ticks_to(&f, 500, hp), hp);
        dim_fader_hold_start(&f, +1, 59, 1000);         /* hold up from lit: 500 of 1000 at 3300 ms */
        int n = 0;
        while (f.current < 1000 && n < 5000) {
            dim_fader_tick(&f);
            n++;
        }
        hold_ms[k] = ms_of(n, hp);
        dim_fader_hold_stop(&f);
        dim_fader_set(&f, 0, 1000);                     /* off, with the soft end and 80 ms at min */
        n = 0;
        at_floor[k] = 0;
        while (f.current && n < 5000) {
            if (dim_fader_tick(&f) == 59) {
                at_floor[k]++;
            }
            n++;
        }
        off_ms[k] = ms_of(n, hp);
    }
    CHECK(on_ms[0] >= 884 && on_ms[0] <= 904);          /* 0.941 * 950 = 894 */
    CHECK(on_ms[1] >= 884 && on_ms[1] <= 904);
    CHECK(plain_ms[0] >= 990 && plain_ms[0] <= 1010);
    CHECK(plain_ms[1] >= 990 && plain_ms[1] <= 1010);
    CHECK(hold_ms[0] >= 1640 && hold_ms[0] <= 1660);    /* 0.5 * 3300 = 1650 */
    CHECK(hold_ms[1] >= 1640 && hold_ms[1] <= 1660);
    CHECK(off_ms[1] >= off_ms[0] - 10 && off_ms[1] <= off_ms[0] + 10);   /* same time, either mains */
    CHECK(at_floor[0] * 10 >= 80 && at_floor[0] * 10 < 90);              /* 80 ms at the min */
    CHECK(ms_of(at_floor[1], 417) >= 80 && ms_of(at_floor[1], 417) < 89);
}

static void test_half_period_outside_40_70_hz_is_50_hz(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set_half_period(&f, 100);
    CHECK(f.half_period == 500);
    dim_fader_set_half_period(&f, 2000);
    CHECK(f.half_period == 500);
    dim_fader_set_half_period(&f, 417);
    CHECK(f.half_period == 417);
}

static void test_zc_locks_on_60_hz_and_follows_a_change(void) {
    zc_tracker_t z;
    zc_tracker_init(&z);
    uint16_t t = 1000;
    zc_tracker_edge(&z, t);
    for (int i = 0; i < ZC_LOCK_EDGES; i++) {
        t = (uint16_t)(t + 417);
        CHECK(zc_tracker_edge(&z, t) == 1);
    }
    CHECK(zc_locked(&z));
    CHECK(z.half_period >= 416 && z.half_period <= 418);
    for (int i = 0; i < 10; i++) {                      /* bounce and mid-cycle noise ignored */
        t = (uint16_t)(t + 417);
        CHECK(zc_tracker_edge(&z, t) == 1);
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + 3)) == 0);
        CHECK(zc_tracker_edge(&z, (uint16_t)(t + 200)) == 0);
    }
    CHECK(z.half_period >= 416 && z.half_period <= 418);
    /* the mains changes to 50 Hz (generator, travel): lock is regained there */
    int acted = 0;
    for (int i = 0; i < 40; i++) {
        t = (uint16_t)(t + 500);
        zc_tracker_check(&z, t);
        acted += zc_tracker_edge(&z, t);
    }
    CHECK(zc_locked(&z));
    CHECK(z.half_period >= 499 && z.half_period <= 501);
    CHECK(acted > 30);
}


/* ---- transition time (dim_fader_set_timed) ---------------------------------- */

/* Half cycles until the fader reaches v (0 if not within max). */
static int timed_ticks(dim_fader_t *f, uint16_t v, int max) {
    for (int n = 1; n <= max; n++) {
        if (dim_fader_tick(f) == v) {
            return n;
        }
    }
    return 0;
}

static void test_timed_lit_to_lit_takes_the_time_at_one_rate(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 200);
    timed_ticks(&f, 200, 1000);
    dim_fader_set_timed(&f, 1, 800, 3000);      /* 600 steps in 3 s: 300 half cycles at 50 Hz */
    for (int i = 0; i < 150; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current >= 498 && f.current <= 502);   /* half way at half the time: even */
    int n = timed_ticks(&f, 800, 1000);
    CHECK(n >= 149 && n <= 151);
    dim_fader_set_timed(&f, 1, 300, 500);       /* down: 500 steps in 0.5 s */
    n = timed_ticks(&f, 300, 1000);
    CHECK(n >= 49 && n <= 51);
}

static void test_timed_from_dark_starts_at_the_floor(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set_floor(&f, 100);
    dim_fader_set_timed(&f, 1, 600, 2000);      /* floor 100 -> 600 in 2 s */
    CHECK(dim_fader_tick(&f) >= 100);
    int n = timed_ticks(&f, 600, 1000);
    CHECK(n >= 197 && n <= 200);
}

static void test_timed_off_reaches_the_floor_in_the_time_then_goes_dark(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set_floor(&f, 100);
    dim_fader_set(&f, 1, 600);
    timed_ticks(&f, 600, 1000);
    dim_fader_set_timed(&f, 0, 600, 1000);      /* dark after 1 s: the floor, a moment there, out */
    int n = timed_ticks(&f, 100, 1000);
    CHECK(n >= 91 && n <= 93);                  /* 1000 - 80 ms */
    int m = timed_ticks(&f, 0, 100);
    CHECK(n + m >= 99 && n + m <= 102);
}

static void test_timed_zero_is_the_profile(void) {
    dim_fader_t a, b;
    dim_fader_init(&a);
    dim_fader_init(&b);
    dim_fader_set(&a, 1, 1000);
    dim_fader_set_timed(&b, 1, 1000, 0);
    CHECK(b.timed_left == 0);
    CHECK(timed_ticks(&a, 1000, 10000) == timed_ticks(&b, 1000, 10000));
}

static void test_timed_ends_with_a_new_target_or_a_hold_and_stays_for_the_same_target(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 100);
    timed_ticks(&f, 100, 1000);
    dim_fader_set_timed(&f, 1, 900, 8000);      /* slow: 1 step per 10 ms */
    for (int i = 0; i < 10; i++) {
        dim_fader_tick(&f);
    }
    dim_fader_set(&f, 1, 900);                  /* the same target (a setting written): kept */
    CHECK(f.timed_left != 0);
    dim_fader_set_timed(&f, 1, 900, 8000);      /* the same target timed again: no new start */
    int n = timed_ticks(&f, 900, 1000);
    CHECK(n >= 788 && n <= 791);                /* 8 s from the first, 10 half cycles gone */
    dim_fader_set_timed(&f, 1, 100, 8000);
    dim_fader_tick(&f);
    dim_fader_set(&f, 1, 950);                  /* a new target without a time: the profile */
    CHECK(f.timed_left == 0);
    dim_fader_set_timed(&f, 1, 200, 5000);
    dim_fader_hold_start(&f, +1, 10, 1000);     /* the finger wins */
    CHECK(f.timed_left == 0);
}

static void test_timed_holds_on_60_hz(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set_half_period(&f, 417);         /* 60 Hz: 8.33 ms half cycles */
    dim_fader_set(&f, 1, 100);
    timed_ticks(&f, 100, 1000);
    dim_fader_set_timed(&f, 1, 700, 3000);      /* 3 s = 360 half cycles at 60 Hz */
    int n = timed_ticks(&f, 700, 2000);
    CHECK(n >= 358 && n <= 362);
}

static void test_timed_extremes(void) {
    dim_fader_t f;
    dim_fader_init(&f);
    dim_fader_set(&f, 1, 500);
    timed_ticks(&f, 500, 1000);
    dim_fader_set_timed(&f, 1, 501, 0xFFFF);    /* 1 step in 65.5 s: takes that long */
    int n = timed_ticks(&f, 501, 10000);
    CHECK(n >= 6552 && n <= 6554);
    dim_fader_set_timed(&f, 1, 510, 10000);     /* 9 steps in 10 s, evenly */
    for (int i = 0; i < 500; i++) {
        dim_fader_tick(&f);
    }
    CHECK(f.current >= 505 && f.current <= 506);
    n = timed_ticks(&f, 510, 10000);
    CHECK(n >= 499 && n <= 501);
    dim_fader_set_timed(&f, 1, 1000, 1);        /* under a half cycle: at once */
    CHECK(dim_fader_tick(&f) == 1000);
    dim_fader_set_timed(&f, 1, 1000, 3000);     /* nowhere to go: the profile */
    CHECK(f.timed_left == 0);
}


/* ---- 2.2.0: start boost -------------------------------------------------------------- */

static void boosted(dim_fader_t *f, uint16_t ms) {
    dim_profile_t p;
    dim_fader_init(f);
    dim_fader_set_floor(f, 100);
    dim_profile_default(&p);
    p.boost_ms = ms;
    dim_fader_set_profile(f, &p);
}

static void test_boost_at_full_then_straight_to_the_level(void) {
    dim_fader_t f;
    boosted(&f, 200);
    dim_fader_set(&f, 1, 300);
    for (int i = 0; i < 19; i++) {
        CHECK(dim_fader_tick(&f) == 1000);       /* 200 ms at full (20 half cycles) */
    }
    dim_fader_tick(&f);
    CHECK(f.current == 300);                     /* then the level at once */
    dim_fader_set(&f, 0, 300);
    timed_ticks(&f, 0, 1000);
    dim_fader_set(&f, 1, 300);                   /* dark again: a boost again */
    CHECK(dim_fader_tick(&f) == 1000);
}

static void test_boost_goes_on_through_the_level_after_on_and_ends_with_off_or_a_hold(void) {
    dim_fader_t f;
    boosted(&f, 300);
    dim_fader_set(&f, 1, 1000);                  /* the module: on (the old level) ... */
    dim_fader_tick(&f);
    dim_fader_set(&f, 1, 250);                   /* ... then its level, 3 ms later */
    int n = timed_ticks(&f, 250, 100);
    CHECK(n >= 28 && n <= 30);                   /* the boost went on to its end */
    boosted(&f, 300);
    dim_fader_set(&f, 1, 400);
    dim_fader_tick(&f);
    dim_fader_set(&f, 0, 400);                   /* off during it: out from the floor */
    CHECK(f.boost_left == 0 && f.current == 100);
    n = timed_ticks(&f, 0, 50);
    CHECK(n >= 1 && n <= DIM_OFF_HOLD_MS / 10 + 1);
    boosted(&f, 300);
    dim_fader_set(&f, 1, 400);
    dim_fader_hold_start(&f, +1, 100, 1000);         /* a hold from a level: no boost */
    CHECK(f.boost_left == 0);
    boosted(&f, 300);
    dim_fader_set(&f, 1, 400);
    dim_fader_hold_start_from_dark(&f, 100, 1000);   /* from dark: the kick, then from the min */
    CHECK(f.boost_left > 0);
    n = 0;
    while (f.boost_left && n < 100) {
        CHECK(dim_fader_tick(&f) == DIM_VALUE_MAX || !f.boost_left);
        n++;
    }
    CHECK(n >= 28 && n <= 30 && f.current == 100 && f.hold_dir == 1);
    dim_fader_tick(&f);
    CHECK(f.current >= 100 && f.current < 120);      /* the hold rises from the min */
}

static void test_a_hold_from_dark_let_go_during_the_boost_stays_at_the_min(void) {
    dim_fader_t f;
    boosted(&f, 600);                            /* longer than the 400 ms a hold takes */
    dim_fader_set(&f, 1, 400);
    dim_fader_hold_start_from_dark(&f, 100, 1000);
    for (int i = 0; i < 45; i++) {
        CHECK(dim_fader_tick(&f) == DIM_VALUE_MAX);      /* 450 ms: the boost still runs */
    }
    CHECK(dim_fader_hold_stop(&f) == 100);       /* the hold's level: its min, not the boost's full */
    CHECK(f.hold_dir == 0 && f.target == 100);
    int n = 0;
    while (f.boost_left && n < 100) {
        CHECK(dim_fader_tick(&f) == DIM_VALUE_MAX || !f.boost_left);
        n++;
    }
    CHECK(n >= 14 && n <= 16);                   /* the kick runs on to its end (150 ms more) */
    CHECK(f.current == 100);                     /* then the min */
    CHECK(dim_fader_tick(&f) == 100);            /* and it stays there */
}

static void test_a_boost_does_not_lengthen_a_timed_on(void) {
    dim_fader_t f;
    boosted(&f, 300);
    dim_fader_set_timed(&f, 1, 600, 1000);           /* on from dark in 1 s, boost 0.3 s */
    int n = 0;
    while ((f.timed_left || f.boost_left) && n < 1000) {
        dim_fader_tick(&f);
        n++;
    }
    CHECK(n >= 99 && n <= 101 && f.current == 600);  /* 1 s in all (10 ms ticks) */
}

static void test_no_boost_from_lit_or_when_off_and_a_timed_on_after_it(void) {
    dim_fader_t f;
    boosted(&f, 0);
    dim_fader_set(&f, 1, 300);
    CHECK(f.boost_left == 0);                    /* none set */
    boosted(&f, 200);
    dim_fader_set(&f, 1, 300);
    timed_ticks(&f, 300, 100);
    dim_fader_set(&f, 1, 700);                   /* lit to lit: no boost */
    CHECK(f.boost_left == 0);
    boosted(&f, 100);
    dim_fader_set_timed(&f, 1, 600, 1000);       /* boost, then the timed rise where it is by now */
    for (int i = 0; i < 9; i++) {
        CHECK(dim_fader_tick(&f) == 1000);
    }
    uint16_t at = dim_fader_tick(&f);
    CHECK(at >= 140 && at <= 160);               /* 0.1 s of 1 s from the floor 100 to 600 */
    int n = timed_ticks(&f, 600, 1000);
    CHECK(n >= 89 && n <= 91);                   /* the time stays 1 s in all */
}

/* Lamp compensation: the duty to take off after the light came down, never while it rises. */
static void test_lamp_compensation(void) {
    const uint16_t lo = 2760;                         /* the min's duty: none from 3760 */
    CHECK(dim_comp(2760, 2760, 320, lo) == 0);        /* on from dark / rising: none */
    CHECK(dim_comp(3000, 3000, 320, lo) == 0);
    CHECK(dim_comp(2760, 6000, 320, lo) == 320);      /* came down from far: all of it at the min */
    CHECK(dim_comp(3260, 6000, 320, lo) == 160);      /* half way up its span */
    CHECK(dim_comp(3700, 6000, 320, lo) == 19);       /* 320 * 60 / 1000 */
    CHECK(dim_comp(3760, 6000, 320, lo) == 0);        /* a tenth of the half cycle up: none */
    CHECK(dim_comp(5000, 7000, 320, lo) == 0);
    CHECK(dim_comp(2760, 2810, 320, lo) == 160);      /* a fall of 50 of 100 */
    CHECK(dim_comp(2760, 6000, 0, lo) == 0);          /* 0: none */
    CHECK(dim_comp(0, 6000, 320, lo) == 0);           /* dark */
    CHECK(dim_comp(2000, 6000, 320, lo) == 0);        /* below the min (fading in or out): none */
    CHECK(dim_comp(100, 10000, 2520, 100) == 2520);   /* the largest: no overflow */
    /* continuous: every step down by one moves it by at most 3 (2520 / 1000) */
    uint16_t prev = dim_comp(3759, 9000, 2520, lo);
    for (uint16_t d = 3758; d >= lo; d--) {
        uint16_t c = dim_comp(d, 9000, 2520, lo);
        CHECK(c >= prev && c - prev <= 3);
        prev = c;
    }
}

int main(void) {
    test_lamp_compensation();
    test_fade_up_takes_two_seconds();
    test_fade_lands_exactly_on_target();
    test_off_fades_to_zero_and_keeps_nothing();
    test_level_clamped();
    test_compare();
    test_curves();
    test_curve_tables_never_fall();
    test_curve_unsigned_maths_gives_the_same_duty();
    test_output_linear_is_unchanged();
    test_output_min_max_are_physical_for_every_curve();
    test_fader_floor();
    test_hold_from_dark_and_cancel();
    test_fade_profile();
    test_times_in_ms_hold_on_50_and_60_hz();
    test_half_period_outside_40_70_hz_is_50_hz();
    test_zc_locks_on_60_hz_and_follows_a_change();
    test_zc_locks_after_regular_edges();
    test_zc_ignores_bounce();
    test_zc_gap_drops_lock();
    test_zc_follows_frequency_drift();
    test_zc_glitch_mid_cycle_is_ignored_when_locked();
    test_zc_edge_within_window_is_followed();
    test_zc_late_edge_relocks_without_output();
    test_zc_lock_takes_four_good_half_cycles();
    test_zc_window_edges_when_locked();
    test_zc_window_from_the_first_good_half_cycle();
    test_zc_noise_on_the_lock_completing_edge();
    test_zc_noise_train_near_the_edge_does_not_capture_a_lock();
    test_zc_noise_inside_the_window_costs_one_half_cycle();
    test_zc_noise_inside_the_window_does_not_move_the_window();
    test_zc_late_isr_moves_the_window_by_a_quarter_of_its_delay();
    test_zc_extra_edge_limits();
    test_zc_pulse_just_before_the_lock_completing_edge_restarts_the_count();
    test_zc_bounce_does_not_stop_the_learning();
    test_zc_extra_edge_after_every_zero_cross_never_locks();
    test_zc_half_period_never_below_the_minimum();
    test_zc_late_edge_inside_the_gap_limit_relocks();
    test_zc_asymmetric_detector();
    test_zc_kinds_at_the_8_percent_limit();
    test_output_max_clamps_for_every_curve();
    test_hold_time_zero_is_the_100_ms_floor();
    test_hold_follows_without_lag_and_freezes_on_release();
    test_hold_stops_at_bounds();
    test_hold_up_from_dark_starts_at_min();
    test_hold_is_slow_just_above_the_min();
    test_hold_up_from_a_lit_level_is_not_slowed();
    test_remote_write_during_hold();
    test_off_during_hold_then_release_stays_off();
    test_timed_lit_to_lit_takes_the_time_at_one_rate();
    test_timed_from_dark_starts_at_the_floor();
    test_timed_off_reaches_the_floor_in_the_time_then_goes_dark();
    test_timed_zero_is_the_profile();
    test_timed_ends_with_a_new_target_or_a_hold_and_stays_for_the_same_target();
    test_timed_holds_on_60_hz();
    test_timed_extremes();
    test_boost_at_full_then_straight_to_the_level();
    test_boost_goes_on_through_the_level_after_on_and_ends_with_off_or_a_hold();
    test_no_boost_from_lit_or_when_off_and_a_timed_on_after_it();
    test_a_hold_from_dark_let_go_during_the_boost_stays_at_the_min();
    test_a_boost_does_not_lengthen_a_timed_on();
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    printf("dim_core: all tests passed\n");
    return EXIT_SUCCESS;
}
