#include "dim_core.h"
#include "curves.h"

void dim_fader_init(dim_fader_t *f) {
    f->current   = 0;
    f->target    = 0;
    f->step      = DIM_STEP_DEFAULT;
    f->hold_dir  = 0;
    f->hold_rate = DIM_HOLD_RATE;
    f->hold_lo   = 0;
    f->hold_hi   = DIM_VALUE_MAX;
    f->floor     = 0;
    f->floor_hold = 0;
    f->from_dark  = 0;
    dim_fader_set_tuning(f, 0);
    dim_fader_set_hold_tuning(f, 0);
    f->hold_acc = 0;
}

void dim_fader_set_hold_tuning(dim_fader_t *f, uint32_t packed) {
    if (packed == 0) {
        packed = DIM_HOLD_TUNING_DEFAULT;
    }
    f->hold_rate_cfg  = (uint8_t)packed;
    f->hold_ease_span = (uint16_t)(((packed >> 8) & 0xFFu) * 4u);
    f->hold_bottom_q  = (uint8_t)(packed >> 16);
}

void dim_fader_set_tuning(dim_fader_t *f, uint32_t packed) {
    if (packed == 0) {
        packed = DIM_TUNING_DEFAULT;
    }
    uint8_t on = (uint8_t)packed, off = (uint8_t)(packed >> 8);
    f->step_on   = on ? on : 1;
    f->step_off  = off ? off : 1;
    f->ease_span = (uint16_t)(((packed >> 16) & 0xFFu) * 4u);
    f->off_hold  = (uint8_t)(packed >> 24);
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
    f->target = on ? level : 0;
}

void dim_fader_hold_start(dim_fader_t *f, int8_t dir, uint8_t rate, uint16_t lo, uint16_t hi) {
    if (hi > DIM_VALUE_MAX) {
        hi = DIM_VALUE_MAX;
    }
    if (lo > hi) {
        lo = hi;
    }
    f->hold_lo   = lo;
    f->hold_hi   = hi;
    f->hold_rate = f->hold_rate_cfg ? f->hold_rate_cfg : (rate ? rate : 1);
    f->hold_acc  = 0;
    f->target    = f->current;
    f->hold_dir  = dir < 0 ? -1 : 1;
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
    f->from_dark = 1;               /* the light is coming on: the quick step */
}

/* Hold step at value cur: the full rate, slower just above the min (in
 * quarter steps, so below one per half cycle too). */
static uint16_t hold_step(dim_fader_t *f, uint16_t cur) {
    uint16_t full_q = (uint16_t)(f->hold_rate * 4u);
    uint16_t q      = full_q;
    if (f->hold_ease_span && f->hold_bottom_q < full_q) {
        uint16_t d = cur > f->hold_lo ? (uint16_t)(cur - f->hold_lo) : 0;
        if (d < f->hold_ease_span) {
            q = (uint16_t)(f->hold_bottom_q +
                           (uint32_t)(full_q - f->hold_bottom_q) * d / f->hold_ease_span);
        }
    }
    f->hold_acc = (uint16_t)(f->hold_acc + q);
    uint16_t step = (uint16_t)(f->hold_acc / 4u);
    f->hold_acc = (uint16_t)(f->hold_acc % 4u);
    return step;
}

static uint16_t hold_tick(dim_fader_t *f) {
    uint16_t cur = f->current;

    if (f->hold_dir > 0) {
        if (cur < f->hold_lo) {
            cur = f->hold_lo;               /* coming up from dark: start at min */
        }
        uint16_t step = hold_step(f, cur);
        cur = (uint16_t)(f->hold_hi - cur > step ? cur + step : f->hold_hi);
    } else {
        if (cur > f->hold_hi) {
            cur = f->hold_hi;
        }
        uint16_t step = hold_step(f, cur);
        cur = (uint16_t)(cur - f->hold_lo > step ? cur - step : f->hold_lo);
    }
    f->current = cur;
    f->target  = cur;
    return cur;
}

/* Fade step at value cur on the way to dark: full above the ease span, down
 * to 1 at the floor (no easing without a floor: floor 0 is the plain fade). */
static uint16_t eased_step(const dim_fader_t *f, uint16_t cur, uint16_t step) {
    if (f->floor == 0 || f->ease_span == 0 || cur >= f->floor + f->ease_span) {
        return step;
    }
    uint16_t d = cur > f->floor ? (uint16_t)(cur - f->floor) : 0;
    return (uint16_t)(1 + (uint32_t)(step - 1) * d / f->ease_span);
}

uint16_t dim_fader_tick(dim_fader_t *f) {
    if (f->hold_dir) {
        return hold_tick(f);
    }
    uint16_t cur = f->current, tgt = f->target, step = f->step ? f->step : 1;

    if (cur < tgt) {
        f->floor_hold = 0;
        if (cur < f->floor) {
            cur          = f->floor;        /* light comes on at the lowest lit level */
            f->from_dark = 1;
        }
        if (f->from_dark) {
            step = f->step_on;              /* on is quick */
        }
        cur = (uint16_t)(tgt - cur > step ? cur + step : tgt);
        if (cur == tgt) {
            f->from_dark = 0;
        }
    } else if (cur > tgt) {
        f->from_dark = 0;
        if (tgt < f->floor && cur <= f->floor) {
            /* At the floor on the way to dark: stay a moment, then out
             * (below the floor the lamp is dark anyway). */
            if (++f->floor_hold >= f->off_hold) {
                f->floor_hold = 0;
                cur           = tgt;
            }
        } else {
            if (tgt < f->floor) {
                step = eased_step(f, cur, f->step_off);   /* going dark: even dim end */
                cur  = (uint16_t)(cur - f->floor > step ? cur - step : f->floor);
            } else {
                cur = (uint16_t)(cur - tgt > step ? cur - step : tgt);
            }
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
    int32_t         a = k[seg], b = k[seg + 1];
    int32_t         v = a + ((b - a) * (int32_t)frac + DIM_VALUE_MAX / 2) / DIM_VALUE_MAX;

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
}

uint8_t zc_tracker_edge(zc_tracker_t *z, uint16_t now) {
    if (!z->started) {
        z->started = 1;
        z->last    = now;
        return 0;
    }
    uint16_t dt = (uint16_t)(now - z->last);

    if (zc_locked(z)) {
        /* Locked: a real zero cross comes within a few percent of where it
         * is expected. Anything earlier is noise (a glitch taken as a zero
         * cross would turn the load on mid cycle, at high voltage). Later
         * means edges were missed: count again from this one, no output
         * until locked again. */
        uint16_t win = (uint16_t)(z->half_period / ZC_WINDOW_DIV);
        if (dt + win < z->half_period) {
            return 0;
        }
        if (dt > z->half_period + win) {
            z->last = now;
            z->good = 0;
            return 0;
        }
    } else if (dt < ZC_HALF_MIN_TICKS) {
        return 0;                       /* bounce or noise: wait for the real edge */
    }
    z->last = now;
    if (dt > ZC_HALF_MAX_TICKS) {
        z->good        = 0;             /* a gap: start counting again */
        z->half_period = 0;
        return 0;
    }
    if (z->good == 0 || z->half_period == 0) {
        z->half_period = dt;
    } else {
        /* Mains frequency drifts slowly; one late edge must not shift the
         * timing much. 1/4 weight for the new sample. */
        z->half_period = (uint16_t)((3u * z->half_period + dt + 2u) / 4u);
    }
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
