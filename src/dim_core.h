/* Hardware-free part of the phase-control engine, unit tested on the PC
 * (tests/test_dim_core.c). Times are in ticks of 20 us (TIM16 / TIM1 with
 * PSC 479 at 24 MHz), so a 50 Hz half cycle is 500 ticks.
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
#define DIM_STEP_DEFAULT       5      /* per half cycle: 0 -> 1000 in 2 s at 50 Hz */

/* Accepted half period: 40..70 Hz mains. Edges closer than the minimum are
 * noise or bounce and are ignored. */
#define ZC_HALF_MIN_TICKS      357    /* 70 Hz */
#define ZC_HALF_MAX_TICKS      625    /* 40 Hz */
#define ZC_LOCK_EDGES          4      /* good half cycles in a row before output */
#define ZC_WINDOW_DIV          12     /* once locked, edges only within +-1/12 (8 %) */

#define DIM_HOLD_RATE          3      /* per half cycle while held: 1000 in 3.3 s */
#define DIM_HOLD_EASE_SPAN     200    /* hold: just above the min it moves slower, so a  */
#define DIM_HOLD_BOTTOM_Q      2      /* very dim level can be picked: at the min 2/4 per */
                                      /* half cycle, full rate DIM_HOLD_EASE_SPAN above  */
#define DIM_STEP_ON            10     /* per half cycle when coming on from dark: quick  */
#define DIM_STEP_OFF           10     /* per half cycle going dark (above the ease span) */
#define DIM_EASE_SPAN          120    /* going dark: just above the floor the steps      */
                                      /* shrink to 1, the dim end fades out softly        */
#define DIM_OFF_HOLD           8      /* half cycles at the floor before going dark      */

typedef struct {
    uint16_t current;   /* value applied now, 0..1000 */
    uint16_t target;    /* 0 when off */
    uint8_t  step;
    int8_t   hold_dir;  /* 0: normal fading; +1/-1: hold dimming */
    uint8_t  hold_rate;
    uint16_t hold_lo;
    uint16_t hold_hi;
    uint16_t floor;     /* lowest lit value (the channel min): fades jump 0 <-> floor */
    uint8_t  floor_hold; /* half cycles spent at the floor on the way to dark */
    uint8_t  from_dark;  /* this rise started dark: the quick on step */
    /* Fade profile (dim_fader_set_tuning): steps per half cycle coming on from
     * dark / going dark, ease span above the floor, half cycles at the floor. */
    uint8_t  step_on;
    uint8_t  step_off;
    uint16_t ease_span;
    uint8_t  off_hold;
    /* Hold profile (dim_fader_set_hold_tuning). */
    uint8_t  hold_rate_cfg;   /* 0: the rate given to hold_start */
    uint8_t  hold_bottom_q;   /* quarter steps per half cycle at the min */
    uint16_t hold_ease_span;  /* 0: no slow-down */
    uint16_t hold_acc;        /* quarter steps not yet moved */
} dim_fader_t;

#define DIM_HOLD_TUNING_DEFAULT ((uint32_t)0 | (uint32_t)(DIM_HOLD_EASE_SPAN / 4) << 8 | \
                                 (uint32_t)DIM_HOLD_BOTTOM_Q << 16)

#define DIM_TUNING_DEFAULT ((uint32_t)DIM_STEP_ON | (uint32_t)DIM_STEP_OFF << 8 | \
                            (uint32_t)(DIM_EASE_SPAN / 4) << 16 | (uint32_t)DIM_OFF_HOLD << 24)

void     dim_fader_init(dim_fader_t *f);
/** Target for this channel: 0 when off, else the level (10..1000). */
void     dim_fader_set(dim_fader_t *f, uint8_t on, uint16_t level);
/** Lowest lit value of the channel (its min). Turning on starts there and
 * turning off leaves from there: below it the lamp is dark anyway, so a fade
 * through 0..floor would only show as a delay. */
void     dim_fader_set_floor(dim_fader_t *f, uint16_t floor);
/** Fade profile, packed: byte 0 step on from dark, 1 step going dark, 2 ease
 * span / 4 (0 = no easing), 3 half cycles at the floor before dark; 0 = the
 * defaults (DIM_TUNING_DEFAULT). Steps of 0 count as 1. */
void     dim_fader_set_tuning(dim_fader_t *f, uint32_t packed);
/** Hold profile, packed: byte 0 rate per half cycle (0 = the caller's), 1 slow-down
 * span above the min / 4 (0 = none), 2 quarter steps per half cycle at the min;
 * 0 = the defaults (DIM_HOLD_TUNING_DEFAULT). */
void     dim_fader_set_hold_tuning(dim_fader_t *f, uint32_t packed);
/** One half cycle: move current towards target by step, return current. */
uint16_t dim_fader_tick(dim_fader_t *f);

/** Hold dimming: from the value the light has now, move by rate per half cycle
 * towards hi (dir +1) or lo (dir -1) and stop there. The light IS the value:
 * no fade lag, so it never runs on after release. */
void     dim_fader_hold_start(dim_fader_t *f, int8_t dir, uint8_t rate, uint16_t lo, uint16_t hi);
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
