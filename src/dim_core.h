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

#define DIM_HOLD_RATE          3      /* per half cycle while held: 1000 in 3.3 s */

typedef struct {
    uint16_t current;   /* value applied now, 0..1000 */
    uint16_t target;    /* 0 when off */
    uint8_t  step;
    int8_t   hold_dir;  /* 0: normal fading; +1/-1: hold dimming */
    uint8_t  hold_rate;
    uint16_t hold_lo;
    uint16_t hold_hi;
} dim_fader_t;

void     dim_fader_init(dim_fader_t *f);
/** Target for this channel: 0 when off, else the level (10..1000). */
void     dim_fader_set(dim_fader_t *f, uint8_t on, uint16_t level);
/** One half cycle: move current towards target by step, return current. */
uint16_t dim_fader_tick(dim_fader_t *f);

/** Hold dimming: from the value the light has now, move by rate per half cycle
 * towards hi (dir +1) or lo (dir -1) and stop there. The light IS the value:
 * no fade lag, so it never runs on after release. */
void     dim_fader_hold_start(dim_fader_t *f, int8_t dir, uint8_t rate, uint16_t lo, uint16_t hi);
/** End of hold: freeze where the light is now, back to normal fading.
 * Returns that value (the new level). */
uint16_t dim_fader_hold_stop(dim_fader_t *f);

#define DIM_DUTY_MAX           10000  /* conduction time in 1/10000 of the half cycle */

/** Curve: value 0..1000 -> conduction time 0..DIM_DUTY_MAX. Unknown curve =
 * linear. Value 0 is always 0 (dark). */
uint16_t dim_curve(uint8_t curve, uint16_t value);
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
