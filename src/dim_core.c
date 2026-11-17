#include "dim_core.h"

void dim_fader_init(dim_fader_t *f) {
    f->current = 0;
    f->target  = 0;
    f->step    = DIM_STEP_DEFAULT;
}

void dim_fader_set(dim_fader_t *f, uint8_t on, uint16_t level) {
    if (level > DIM_VALUE_MAX) {
        level = DIM_VALUE_MAX;
    }
    f->target = on ? level : 0;
}

uint16_t dim_fader_tick(dim_fader_t *f) {
    uint16_t cur = f->current, tgt = f->target, step = f->step ? f->step : 1;

    if (cur < tgt) {
        cur = (uint16_t)(tgt - cur > step ? cur + step : tgt);
    } else if (cur > tgt) {
        cur = (uint16_t)(cur - tgt > step ? cur - step : tgt);
    }
    f->current = cur;
    return cur;
}

uint16_t dim_compare(uint16_t half_period, uint16_t value) {
    if (value > DIM_VALUE_MAX) {
        value = DIM_VALUE_MAX;
    }
    return (uint16_t)(((uint32_t)half_period * value + DIM_VALUE_MAX / 2) / DIM_VALUE_MAX);
}

uint16_t dim_reload(uint16_t half_period) {
    return (uint16_t)(half_period + half_period / 16);
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

    if (dt < ZC_HALF_MIN_TICKS) {
        return 0;                       /* bounce or noise: wait for the real edge */
    }
    z->last = now;
    if (dt > ZC_HALF_MAX_TICKS) {
        z->good        = 0;             /* a gap: start counting again */
        z->half_period = 0;
        return 0;
    }
    if (z->half_period == 0) {
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
