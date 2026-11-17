/* PC tests for the hardware-free dimming logic: make test */
#include "dim_core.h"
#include "curves.h"

/* the default profile in whole steps (quarter steps / 4) */
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
    /* times: 0 = at once, slow times keep their fraction (quarter steps) */
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
    test_hold_follows_without_lag_and_freezes_on_release();
    test_hold_stops_at_bounds();
    test_hold_up_from_dark_starts_at_min();
    test_hold_is_slow_just_above_the_min();
    test_hold_up_from_a_lit_level_is_not_slowed();
    test_remote_write_during_hold();
    test_off_during_hold_then_release_stays_off();
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    printf("dim_core: all tests passed\n");
    return EXIT_SUCCESS;
}
