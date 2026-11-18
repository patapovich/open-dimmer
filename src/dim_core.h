/* Hardware-free part of the phase-control engine, unit tested on the PC
 * (tests/test_dim_core.c). Times are in ticks of 20 us (TIM16 with PSC 479
 * at 24 MHz), so a 50 Hz half cycle is 500 ticks.
 *
 * Model taken from the stock firmware of the twin WiFi board: on every zero
 * cross each channel's current value moves towards its target by a fixed
 * step, and the output conducts from the zero cross (trailing edge). How long
 * it conducts for a value is the channel's curve (curves.h); curve 0 is the
 * stock one, linear in time.
 *
 * TIM1 runs at 2 us (PSC 47), ten times finer than the 20 us time base, so the
 * low end of the perceptual curves has steps to work with. */
#ifndef DIM_CORE_H
#define DIM_CORE_H

#include <stdint.h>

#define DIM_VALUE_MAX          1000
#define DIM_STEP_DEFAULT       5      /* per 10 ms: 0 -> 1000 in 2 s */

/* Accepted half period: 40..70 Hz mains. Edges closer than the minimum are
 * noise or bounce and are ignored. */
#define ZC_HALF_MIN_TICKS      357    /* 70 Hz */
#define ZC_HALF_MAX_TICKS      625    /* 40 Hz */
#define ZC_LOCK_EDGES          4      /* good half cycles in a row before output */
#define ZC_WINDOW_DIV          12     /* once locked, edges only within +-1/12 (8 %) */

/* Default profile (per channel, see dim_profile_t; the user can set them). */
#define DIM_FADE_ON_MS         950    /* dark -> full when switched on: quick         */
#define DIM_FADE_OFF_MS        950    /* full -> dark when switched off               */
#define DIM_SOFT_END_PCT       12     /* switching off: the lowest 12 % above the min  */
                                      /* slow down to 1 step per 10 ms (an even end)   */
#define DIM_HOLD_MS            3300   /* hold dimming over the whole range             */
#define DIM_HOLD_END_PCT       20     /* hold: the lowest 20 % above the min slow down */
#define DIM_Q_PER_STEP         64     /* rates are in 1/64 steps per 10 ms             */
#define DIM_HOLD_BOTTOM_Q      32     /* ... to 1/2 step per 10 ms at the min          */
#define DIM_OFF_HOLD_MS        80     /* at the min before going dark                  */
#define DIM_HALF_PERIOD_50HZ   500    /* 20 us ticks: the rates are per 10 ms half cycle */
#define DIM_HOLD_RATE          3      /* full hold rate per 10 ms at 3300 ms           */
#define DIM_PROFILE_MS_MAX     10000  /* longest fade / hold time a user can set       */

/* How a channel fades and holds. Rates in 1/64 steps per 10 ms (DIM_Q_PER_STEP). */
typedef struct {
    uint16_t on_q;          /* coming on from dark */
    uint16_t off_q;         /* going dark, above the soft end */
    uint16_t soft_span;     /* going dark: values above the floor that slow down (0 = none) */
    uint16_t hold_q;        /* hold, full rate */
    uint16_t hold_span;     /* hold: values above the min that slow down (0 = none) */
    uint8_t  hold_bottom_q; /* hold rate at the min */
    uint8_t  off_hold_ms;   /* at the floor before dark */
} dim_profile_t;

/** Profile from the user settings: times in ms for the whole range (0 = at
 * once), spans in % of the range. */
void     dim_profile_set(dim_profile_t *p, uint16_t fade_on_ms, uint16_t fade_off_ms,
                         uint8_t soft_end_pct, uint16_t hold_ms, uint8_t hold_end_pct);
void     dim_profile_default(dim_profile_t *p);

typedef struct {
    uint16_t      current;   /* value applied now, 0..1000 */
    uint16_t      target;    /* 0 when off */
    uint8_t       step;      /* lit to lit (level changes), steps per 10 ms */
    int8_t        hold_dir;  /* 0: normal fading; +1/-1: hold dimming */
    uint16_t      hold_lo;
    uint16_t      hold_hi;
    uint16_t      floor;     /* lowest lit value (the channel min): fades jump 0 <-> floor */
    uint16_t      floor_hold; /* 20 us ticks spent at the floor on the way to dark */
    uint8_t       from_dark; /* this rise started dark: the on rate */
    uint8_t       hold_fine; /* the hold slows down near the min: going down, or up from dark */
    uint16_t      half_period; /* measured, 20 us ticks (the mains: 500 at 50 Hz, 417 at 60 Hz) */
    uint32_t      acc;       /* 1/64 steps x half period not yet moved (64 x 500 = one step) */
    dim_profile_t prof;
} dim_fader_t;

void     dim_fader_init(dim_fader_t *f);
/** Target for this channel: 0 when off, else the level (10..1000). */
void     dim_fader_set(dim_fader_t *f, uint8_t on, uint16_t level);
/** Lowest lit value of the channel (its min). Turning on starts there and
 * turning off leaves from there: below it the lamp is dark anyway, so a fade
 * through 0..floor would only show as a delay. */
void     dim_fader_set_floor(dim_fader_t *f, uint16_t floor);
void     dim_fader_set_profile(dim_fader_t *f, const dim_profile_t *p);
/** The measured half period (20 us ticks) of the half cycle about to be ticked: the
 * rates are for 10 ms half cycles and scale with it, so the times in ms hold on 50 and
 * 60 Hz. Outside 40..70 Hz it is taken as 50 Hz. */
void     dim_fader_set_half_period(dim_fader_t *f, uint16_t half_period);
/** One half cycle: move current towards target, return current. */
uint16_t dim_fader_tick(dim_fader_t *f);

/** Hold dimming: from the value the light has now, move at the hold rate
 * towards hi (dir +1) or lo (dir -1) and stop there. The light IS the value:
 * no fade lag, so it never runs on after release. */
void     dim_fader_hold_start(dim_fader_t *f, int8_t dir, uint16_t lo, uint16_t hi);
/** Hold that started dark: rising from the min, slowly at first (a very dim
 * level can be picked). A hold up from a lit level moves at the full rate. */
void     dim_fader_hold_start_from_dark(dim_fader_t *f, uint16_t lo, uint16_t hi);
/** End of hold: freeze where the light is now, back to normal fading.
 * Returns that value (the new level). */
uint16_t dim_fader_hold_stop(dim_fader_t *f);
/** End of a hold that was only provisional (a tap from dark): back to normal
 * fading, quickly, towards the target set next (dim_fader_set). */
void     dim_fader_hold_cancel(dim_fader_t *f);

#define DIM_DUTY_MAX           10000  /* conduction time in 1/10000 of the half cycle */

/** Curve: value 0..1000 -> conduction time 0..DIM_DUTY_MAX. Unknown curve =
 * linear. Value 0 is always 0 (dark). */
uint16_t dim_curve(uint8_t curve, uint16_t value);
/** Output duty for a channel: min and max are physical limits of the lamp
 * (conduction time = level/1000 of the half cycle, as the stock firmware and
 * the linear curve mean them); the curve spreads the range between them.
 *   duty = min + curve((value - min) / (max - min)) * (max - min)
 * So min/max keep their meaning whatever the curve, and the linear curve
 * gives exactly value/1000 as before. 0 = dark; below min = min. */
uint16_t dim_output(uint8_t curve, uint16_t value, uint16_t min, uint16_t max);

/** TIM1 compare (2 us ticks) for a duty, half_period in 20 us ticks. */
uint16_t dim_compare(uint16_t half_period, uint16_t duty);
/** TIM1 auto-reload (2 us ticks): a little past the expected next zero cross,
 * so a late edge does not start a new pulse mid cycle (the wrap loads
 * compare 0 instead). */
uint16_t dim_reload(uint16_t half_period);

typedef struct {
    uint16_t last;        /* timestamp of the last accepted edge */
    uint16_t half_period; /* smoothed, 0 = unknown */
    uint8_t  good;        /* consecutive good half cycles, saturates */
    uint8_t  started;
} zc_tracker_t;

void zc_tracker_init(zc_tracker_t *z);
/** Feed an edge timestamp. Returns 1 if the edge is a zero cross to act on
 * (output allowed only once zc_locked()), 0 if it was ignored as noise. */
uint8_t zc_tracker_edge(zc_tracker_t *z, uint16_t now);
/** Locked onto mains: enough regular half cycles in a row. */
uint8_t zc_locked(const zc_tracker_t *z);
/** Called with the time now: drops the lock if edges stopped. Returns 1 when
 * the lock was dropped by this call. */
uint8_t zc_tracker_check(zc_tracker_t *z, uint16_t now);

#endif
