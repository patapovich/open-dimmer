#include "dim_core.h"
#include "curves.h"

static uint16_t rate_q(uint16_t ms) {
    /* the whole range (1000 steps) in ms: 1000 * 64 / (ms / 10) per 10 ms */
    if (ms < 10) {
        return 1000u * DIM_Q_PER_STEP;      /* at once */
    }
    uint32_t q = (1000u * DIM_Q_PER_STEP * 10u + ms / 2u) / ms;
    return (uint16_t)(q < 1u ? 1u : q);
}

void dim_profile_set(dim_profile_t *p, uint16_t fade_on_ms, uint16_t fade_off_ms,
                     uint8_t soft_end_pct, uint16_t hold_ms, uint8_t hold_end_pct) {
    p->on_q          = rate_q(fade_on_ms);
    p->off_q         = rate_q(fade_off_ms);
    p->soft_span     = (uint16_t)((soft_end_pct > 100 ? 100 : soft_end_pct) * 10u);
    p->hold_q        = rate_q(hold_ms < 100 ? 100 : hold_ms);
    p->hold_span     = (uint16_t)((hold_end_pct > 100 ? 100 : hold_end_pct) * 10u);
    p->hold_bottom_q = DIM_HOLD_BOTTOM_Q;
    p->off_hold_ms   = DIM_OFF_HOLD_MS;
}

void dim_profile_default(dim_profile_t *p) {
    dim_profile_set(p, DIM_FADE_ON_MS, DIM_FADE_OFF_MS, DIM_SOFT_END_PCT, DIM_HOLD_MS,
                    DIM_HOLD_END_PCT);
}

void dim_fader_init(dim_fader_t *f) {
    f->current    = 0;
    f->target     = 0;
    f->step       = DIM_STEP_DEFAULT;
    f->hold_dir   = 0;
    f->hold_lo    = 0;
    f->hold_hi    = DIM_VALUE_MAX;
    f->floor      = 0;
    f->floor_hold = 0;
    f->from_dark  = 0;
    f->acc        = 0;
    f->half_period = DIM_HALF_PERIOD_50HZ;
    f->hold_fine  = 0;
    f->timed_left = 0;
    dim_profile_default(&f->prof);
}

void dim_fader_set_profile(dim_fader_t *f, const dim_profile_t *p) {
    f->prof = *p;
}

void dim_fader_set_half_period(dim_fader_t *f, uint16_t half_period) {
    f->half_period = (half_period >= ZC_HALF_MIN_TICKS && half_period <= ZC_HALF_MAX_TICKS)
                     ? half_period : DIM_HALF_PERIOD_50HZ;
}

void dim_fader_set_floor(dim_fader_t *f, uint16_t floor) {
    f->floor = floor > DIM_VALUE_MAX ? DIM_VALUE_MAX : floor;
}

void dim_fader_set(dim_fader_t *f, uint8_t on, uint16_t level) {
    if (level > DIM_VALUE_MAX) {
        level = DIM_VALUE_MAX;
    }
    if (f->hold_dir) {
        if (!on) {
            f->hold_dir = 0;                /* off wins over a hold */
            f->target   = 0;
        }
        return;                             /* the finger is in control */
    }
    uint16_t t = on ? level : 0;
    if (t != f->target) {
        f->timed_left = 0;                  /* a new target: the profile's rates again */
    }
    f->target = t;
}

void dim_fader_set_timed(dim_fader_t *f, uint8_t on, uint16_t level, uint16_t ms) {
    uint16_t before = f->target;
    dim_fader_set(f, on, level);
    if (f->hold_dir || ms == 0 || (f->target == before && f->timed_left)) {
        return;                             /* no time, or the same move going on */
    }
    /* From where the light is (dark starts at the floor) to the target (going dark: the floor,
     * the moment there counted in). */
    uint16_t from = f->current < f->floor ? f->floor : f->current;
    uint16_t to   = f->target < f->floor ? f->floor : f->target;
    if (from == to) {
        return;
    }
    if (f->target == 0 && ms > f->prof.off_hold_ms) {
        ms = (uint16_t)(ms - f->prof.off_hold_ms);
    }
    f->timed_from  = from;
    f->timed_to    = to;
    f->timed_total = f->timed_left = (uint32_t)ms * 50u;   /* 50 ticks = 1 ms */
}

void dim_fader_hold_start(dim_fader_t *f, int8_t dir, uint16_t lo, uint16_t hi) {
    if (hi > DIM_VALUE_MAX) {
        hi = DIM_VALUE_MAX;
    }
    if (lo > hi) {
        lo = hi;
    }
    f->hold_lo   = lo;
    f->hold_hi   = hi;
    f->acc       = 0;
    f->target    = f->current;
    f->hold_dir  = dir < 0 ? -1 : 1;
    f->hold_fine  = dir < 0;        /* going down towards the min: fine there */
    f->timed_left = 0;
}

void dim_fader_hold_start_from_dark(dim_fader_t *f, uint16_t lo, uint16_t hi) {
    dim_fader_hold_start(f, +1, lo, hi);
    f->hold_fine = 1;
}

uint16_t dim_fader_hold_stop(dim_fader_t *f) {
    if (f->hold_dir) {              /* an off may have ended the hold already: */
        f->hold_dir = 0;            /* then it keeps fading to its target      */
        f->target   = f->current;
    }
    return f->current;
}

void dim_fader_hold_cancel(dim_fader_t *f) {
    f->hold_dir  = 0;
    f->target    = f->current;
    f->from_dark = 1;               /* the light is coming on: the on rate */
    f->acc       = 0;
}

/* Whole steps to move now for q 1/64 steps (the rest is kept). */
static uint16_t take(dim_fader_t *f, uint16_t q) {
    /* q 1/64 steps per 10 ms, for a half cycle of half_period: exact over time */
    uint32_t a    = f->acc + (uint32_t)q * f->half_period;
    uint32_t unit = (uint32_t)DIM_Q_PER_STEP * DIM_HALF_PERIOD_50HZ;
    f->acc = a % unit;
    return (uint16_t)(a / unit);
}

/* Rate q, slowing to lo_q at distance 0 from the bottom of a span. */
static uint16_t eased_q(uint16_t q, uint16_t lo_q, uint16_t d, uint16_t span) {
    if (span == 0 || d >= span || q <= lo_q) {
        return q;
    }
    return (uint16_t)(lo_q + (uint32_t)(q - lo_q) * d / span);
}

static uint16_t hold_tick(dim_fader_t *f) {
    uint16_t cur = f->current;

    if (f->hold_dir > 0 && cur < f->hold_lo) {
        cur = f->hold_lo;                   /* coming up from dark: start at min */
    } else if (f->hold_dir < 0 && cur > f->hold_hi) {
        cur = f->hold_hi;
    }
    uint16_t d    = cur > f->hold_lo ? (uint16_t)(cur - f->hold_lo) : 0;
    uint16_t q    = f->hold_fine ? eased_q(f->prof.hold_q, f->prof.hold_bottom_q, d, f->prof.hold_span)
                                 : f->prof.hold_q;
    uint16_t step = take(f, q);
    if (f->hold_dir > 0) {
        cur = (uint16_t)(f->hold_hi - cur > step ? cur + step : f->hold_hi);
    } else {
        cur = (uint16_t)(cur - f->hold_lo > step ? cur - step : f->hold_lo);
    }
    f->current = cur;
    f->target  = cur;
    return cur;
}

uint16_t dim_fader_tick(dim_fader_t *f) {
    if (f->hold_dir) {
        return hold_tick(f);
    }
    if (f->timed_left) {
        /* A timed move: the even position for the time gone (exact on 50 and 60 Hz). d <= 1000
         * and the time <= 65535 ms = 3 276 750 ticks: the product fits 32 bits. */
        f->timed_left = f->timed_left > f->half_period ? f->timed_left - f->half_period : 0;
        uint16_t a = f->timed_from, b = f->timed_to;
        uint16_t d = a > b ? (uint16_t)(a - b) : (uint16_t)(b - a);
        uint16_t m = (uint16_t)((uint32_t)d * (f->timed_total - f->timed_left) / f->timed_total);
        f->current    = b > a ? (uint16_t)(a + m) : (uint16_t)(a - m);
        f->floor_hold = 0;
        f->from_dark  = 0;
        f->acc        = 0;
        return f->current;                  /* at its end the target, or the floor going dark */
    }
    uint16_t cur = f->current, tgt = f->target;
    uint16_t plain_q = (uint16_t)((f->step ? f->step : 1) * DIM_Q_PER_STEP);

    if (cur < tgt) {
        f->floor_hold = 0;
        if (cur < f->floor) {
            cur          = f->floor;        /* light comes on at the lowest lit level */
            f->from_dark = 1;
        }
        uint16_t step = take(f, f->from_dark ? f->prof.on_q : plain_q);
        cur = (uint16_t)(tgt - cur > step ? cur + step : tgt);
        if (cur == tgt) {
            f->from_dark = 0;
        }
    } else if (cur > tgt) {
        f->from_dark = 0;
        if (tgt < f->floor && cur <= f->floor) {
            /* At the floor on the way to dark: stay a moment, then out
             * (below the floor the lamp is dark anyway). */
            f->floor_hold = (uint16_t)(f->floor_hold + f->half_period);
            if (f->floor_hold >= (uint32_t)f->prof.off_hold_ms * 50u) {   /* 50 ticks = 1 ms */
                f->floor_hold = 0;
                cur           = tgt;
            }
        } else if (tgt < f->floor) {
            /* going dark: the off rate, slowing to 1 step per 10 ms at the floor */
            uint16_t step = take(f, eased_q(f->prof.off_q, DIM_Q_PER_STEP, (uint16_t)(cur - f->floor),
                                            f->prof.soft_span));
            cur = (uint16_t)(cur - f->floor > step ? cur - step : f->floor);
        } else {
            uint16_t step = take(f, plain_q);
            cur = (uint16_t)(cur - tgt > step ? cur - step : tgt);
        }
    }
    f->current = cur;
    return cur;
}

uint16_t dim_curve(uint8_t curve, uint16_t value) {
    if (value == 0) {
        return 0;
    }
    if (value >= DIM_VALUE_MAX) {
        return DIM_DUTY_MAX;
    }
    if (curve >= CURVE_COUNT) {
        curve = CURVE_LINEAR;
    }
    const uint16_t *k   = k_curves[curve];
    uint32_t        pos = (uint32_t)value * (CURVE_POINTS - 1);   /* in 1/1000 of a segment */
    uint32_t        seg = pos / DIM_VALUE_MAX;
    uint32_t        frac = pos % DIM_VALUE_MAX;
    uint32_t        a = k[seg], b = k[seg + 1];   /* b >= a: the tables never fall (gen_curves.py) */
    uint32_t        v = a + ((b - a) * frac + DIM_VALUE_MAX / 2) / DIM_VALUE_MAX;

    return (uint16_t)(v < 1 ? 1 : v);       /* on is never fully dark */
}

uint16_t dim_output(uint8_t curve, uint16_t value, uint16_t min, uint16_t max) {
    if (value == 0) {
        return 0;
    }
    if (max > DIM_VALUE_MAX) {
        max = DIM_VALUE_MAX;
    }
    if (min > max) {
        min = max;
    }
    if (value < min) {
        value = min;
    }
    if (value > max) {
        value = max;
    }
    if (curve == CURVE_LINEAR || curve >= CURVE_COUNT) {
        return (uint16_t)(value * (DIM_DUTY_MAX / DIM_VALUE_MAX));   /* min + (v - min): exact */
    }
    uint32_t span = (uint32_t)(max - min);
    if (span == 0) {
        return (uint16_t)(min * (DIM_DUTY_MAX / DIM_VALUE_MAX));
    }
    uint32_t pos   = ((uint32_t)(value - min) * DIM_VALUE_MAX + span / 2) / span;   /* 0..1000 */
    uint32_t shape = pos ? dim_curve(curve, (uint16_t)pos) : 0;                      /* 0..10000 */
    uint32_t duty  = (uint32_t)min * (DIM_DUTY_MAX / DIM_VALUE_MAX) +
                     (shape * span + DIM_VALUE_MAX / 2) / DIM_VALUE_MAX;
    return (uint16_t)(duty < 1 ? 1 : duty);
}

uint16_t dim_compare(uint16_t half_period, uint16_t duty) {
    if (duty > DIM_DUTY_MAX) {
        duty = DIM_DUTY_MAX;
    }
    /* half_period * 10 ticks of 2 us, times duty / 10000 */
    return (uint16_t)(((uint32_t)half_period * duty + 500u) / 1000u);
}

uint16_t dim_reload(uint16_t half_period) {
    return (uint16_t)((half_period + half_period / 16u) * 10u);
}

/* ---- zero cross ---------------------------------------------------------- */

void zc_tracker_init(zc_tracker_t *z) {
    z->last        = 0;
    z->half_period = 0;
    z->good        = 0;
    z->started     = 0;
    z->kind        = 0;
    z->hp[0]       = 0;
    z->hp[1]       = 0;
    z->undo        = 0;
    z->extra       = 0;
}

uint8_t zc_tracker_edge(zc_tracker_t *z, uint16_t now) {
    if (!z->started) {
        z->started = 1;
        z->last    = now;
        return 0;
    }
    uint16_t dt = (uint16_t)(now - z->last);
    uint8_t  k  = z->kind;

    if (dt < ZC_HALF_MIN_TICKS) {
        /* Bounce or noise: wait for the real edge. An edge 6..52 ticks after
         * the one taken (later than its bounce, but inside any window) means
         * one of the two is no zero cross, and it may be the one taken (a
         * pulse just before the real edge, inside the window; the real edge
         * then comes here). Acquiring: count again, from this edge, so no
         * such pulse is left in what the lock is built on. Locked: the gates
         * already turned on there, inside the window; neither half cycle next
         * to it is learned (the one that ended there is undone, the one that
         * started there is skipped), so the noise does not move hp[] and the
         * window stays on the real edges. */
        if ((uint16_t)(dt - ZC_BOUNCE_TICKS - 1u) < (uint16_t)(ZC_SUSPECT_TICKS - ZC_BOUNCE_TICKS)) {
            if (!zc_locked(z)) {
                z->last = now;
                z->good = 0;
                return 0;
            }
            z->extra = 1;
            if (z->undo) {
                z->hp[k ^ 1u] = z->undo;
                z->undo       = 0;
            }
        }
        return 0;
    }
    if (z->good) {
        /* A real zero cross comes where the learned half cycles say. Within
         * 1/12 of the last one (the other kind): the two kinds differ by 8 %
         * at most. And once both kinds are measured, within 1/24 of its own
         * kind: this is the window that admits a zero cross, for the third
         * edge on, so for every edge that turns the gates on. Anything earlier
         * is noise (a glitch taken as a zero cross would turn the load on mid
         * cycle, at high voltage). Later means edges were missed: count again
         * from this one, no output until locked again. */
        uint16_t last = z->hp[k ^ 1u];                 /* the last half cycle */
        uint16_t w    = (uint16_t)(last / (unsigned)ZC_WINDOW_DIV);
        uint16_t own  = last, w_own = (uint16_t)(2u * w);   /* its own kind: not known yet */
        if (z->good >= 2) {
            own   = z->hp[k];
            w_own = w;
        }
        if (dt + 2u * w < last || dt + w_own < own) {
            return 0;
        }
        if (dt > last + 2u * w || dt > own + w_own) {
            z->last = now;
            z->good = 0;
            return 0;
        }
    }
    z->last = now;
    if (dt > ZC_HALF_MAX_TICKS) {
        z->good        = 0;             /* a gap: start counting again */
        z->half_period = 0;
        return 0;
    }
    /* Mains frequency drifts slowly; one late edge must not shift the timing
     * much. 1/4 weight for the new sample, once the kind has one; not at all
     * if an extra edge came right after the edge this half cycle started at. */
    uint8_t clean = !z->extra;
    z->extra      = 0;
    z->undo       = 0;
    if (z->good < 2) {
        z->hp[k] = dt;
    } else if (clean) {
        z->undo  = z->hp[k];
        z->hp[k] = (uint16_t)((3u * z->hp[k] + dt + 2u) / 4u);
    }
    k ^= 1u;
    z->kind = k;
    /* The half cycle starting now is the other kind: as long as its last one,
     * or as this one while that is not known yet. */
    z->half_period = z->good ? z->hp[k] : dt;
    if (z->good < 255) {
        z->good++;
    }
    return 1;
}

uint8_t zc_locked(const zc_tracker_t *z) {
    return z->good >= ZC_LOCK_EDGES && z->half_period != 0;
}

uint8_t zc_tracker_check(zc_tracker_t *z, uint16_t now) {
    if (!z->started || z->good == 0) {
        return 0;
    }
    if ((uint16_t)(now - z->last) > ZC_HALF_MAX_TICKS + ZC_HALF_MAX_TICKS / 2) {
        uint8_t was = zc_locked(z);
        z->good        = 0;
        z->half_period = 0;
        return was;
    }
    return 0;
}
