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
 * noise or bounce and are ignored, locked or not. */
#define ZC_HALF_MIN_TICKS      357    /* 70 Hz */
#define ZC_HALF_MAX_TICKS      625    /* 40 Hz */
#define ZC_LOCK_EDGES          4      /* good half cycles in a row before output */
/* Acceptance window. A detector need not be symmetric (on the spare PA4 is
 * high 10.08 ms, low 10.02 ms), so the two kinds of half cycle (PA4 high, PA4
 * low) are timed separately. From the third edge on, a half cycle must be
 * within +-1/24 (4 %, 0.4 ms at 50 Hz) of the learned length of its kind; from
 * the second edge on, also within +-1/12 of the last half cycle, so the two
 * kinds differ by 8 % at most. Every edge that turns the gates on has passed
 * the 1/24 window. The window is measured from the last edge taken, against
 * the learned lengths, so a turn-on can be further from the real zero cross:
 *  - noise less than ZC_BOUNCE_TICKS before the real edge cannot be told from
 *    it and may shift the timing by that much: window + 0.1 ms (0.5 ms,
 *    ~50 V). Noise further inside the window is given away by the real edge
 *    right after it and moves nothing (ZC_SUSPECT_TICKS);
 *  - an ISR held back by d (up to the window) moves the two kinds apart by
 *    d/4 each, fading by 3/4 per period: window + d/4;
 *  - noise inside the window in consecutive half cycles moves the edge taken
 *    by up to a window each time. */
#define ZC_WINDOW_DIV          24
/* An edge up to this soon after the one taken is its bounce: 0.1 ms. A bounce
 * is timed when the zero-cross ISR has finished (a few us while acquiring, up
 * to ~93 us while locked; one read later than this only skips a learning). */
#define ZC_BOUNCE_TICKS        5
/* ... after that and up to this (the widest window, 1/12 of a 40 Hz half
 * cycle: 1.04 ms) the one taken may be noise just before the real edge.
 * Acquiring, the count starts again from the later edge; locked, neither half
 * cycle next to the edge taken is learned. A detector that chatters this long
 * after every edge never locks (dark). */
#define ZC_SUSPECT_TICKS       (ZC_HALF_MAX_TICKS / 12)

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
    uint16_t boost_ms;      /* start boost: at full this long when it comes on from dark (0: none;
                               dim_profile_set leaves it 0) */
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
    /* A timed move (a transition time, dim_fader_set_timed): from -> to over total 20 us ticks,
     * the light at the even position for the time gone; left 0: none (the profile's rates). It
     * ends with a new target or a hold. */
    uint16_t      timed_from;
    uint16_t      timed_to;
    uint32_t      timed_left;
    uint32_t      timed_total;
    uint32_t      boost_left;  /* 20 us ticks of a start boost still to go (at full, then the target) */
    dim_profile_t prof;
} dim_fader_t;

void     dim_fader_init(dim_fader_t *f);
/** Target for this channel: 0 when off, else the level (10..1000). A new target ends a timed
 * move (dim_fader_set_timed); the same target keeps it. */
void     dim_fader_set(dim_fader_t *f, uint8_t on, uint16_t level);
/** As dim_fader_set, but the move to this target takes ms (a Zigbee transition time), at one
 * even rate: from where the light is (the floor when dark) to the target; going dark it reaches
 * the floor so that, with the usual moment there, it is dark after ms. The same target again
 * while it runs keeps it going (it does not start over). ms 0: the profile's rates, as
 * dim_fader_set; under one half cycle: at once. During a hold it does what dim_fader_set does. */
void     dim_fader_set_timed(dim_fader_t *f, uint8_t on, uint16_t level, uint16_t ms);
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

/* The custom curve (2.2.8): a table of CUSTOM_POINTS outputs (0..CURVE_OUT_MAX at 0, 1/16 ..
 * 1 of the range) per channel, written by the module (DPs 174..) and saved with the settings
 * (the record's extension, dimmer.c). Curve CURVE_CUSTOM takes the table of channel
 * dim_custom_ch (phase.c sets it before its dim_output); without a valid table: led_soft.
 * dim_custom_put writes 6 points from `first` (little-endian u16; from 12: 5 and one more) and
 * checks the whole table (dim_custom_check: 0 at the start, CURVE_OUT_MAX at the end, never
 * falling). dim_custom_raw: the channel's CUSTOM_RAW bytes, channel 2's right after channel
 * 1's (the record takes both at once), for the record and the DPs' reports. */
#define CURVE_CUSTOM    6      /* = CURVE_COUNT (curves.h, checked in dim_core.c) */
#define CUSTOM_POINTS   17
#define CUSTOM_RAW      36     /* 3 parts of 6 points, little-endian (the last part's sixth: none) */
void     dim_custom_put(uint8_t ch, uint8_t first, const uint8_t *le16);
uint8_t  dim_custom_ok(uint8_t ch);
void     dim_custom_check(uint8_t ch);
uint8_t *dim_custom_raw(uint8_t ch);
void     dim_custom_load(const uint8_t *both);   /* 2 * CUSTOM_RAW bytes from the record, checked */
extern uint8_t dim_custom_ch;
/** Output duty for a channel: min and max are physical limits of the lamp
 * (conduction time = level/1000 of the half cycle, as the stock firmware and
 * the linear curve mean them); the curve spreads the range between them.
 *   duty = min + curve((value - min) / (max - min)) * (max - min)
 * So min/max keep their meaning whatever the curve, and the linear curve
 * gives exactly value/1000 as before. 0 = dark; below min = min. */
uint16_t dim_output(uint8_t curve, uint16_t value, uint16_t min, uint16_t max);
/* Lamp compensation (2.2.8). Some LED lamps give more light at the same conduction after they
 * have been brighter (their driver keeps a state): coming down to a level they are brighter
 * than when they came up to it or on from dark. All in duty (dim_output, after the curve: a
 * soft curve's flat bottom would swallow a value offset), and in the lamp's own terms, not the
 * user's min..max: the duty is lowered by up to `amount` at the min's duty, linearly less up
 * to DIM_COMP_SPAN above it, none from there; all of it once the duty came down DIM_COMP_FALL
 * from `peak` (the highest duty since the light came on, followed slowly: phase.c), less after
 * a smaller fall, none while it rises. 0: none. Returns the duty to take off (0..amount). */
#define DIM_COMP_SPAN   1000   /* duty above the min's: a tenth of the half cycle (a measured lamp) */
#define DIM_COMP_FALL   100    /* duty fallen from the peak for all of it */
uint16_t dim_comp(uint16_t duty, uint16_t peak, uint16_t amount, uint16_t min_duty);

/** TIM1 compare (2 us ticks) for a duty, half_period in 20 us ticks. */
uint16_t dim_compare(uint16_t half_period, uint16_t duty);
/** TIM1 auto-reload (2 us ticks): a little past the expected next zero cross,
 * so a late edge does not start a new pulse mid cycle (the wrap loads
 * compare 0 instead). */
uint16_t dim_reload(uint16_t half_period);

typedef struct {
    uint16_t last;        /* timestamp of the last accepted edge */
    uint16_t half_period; /* expected length of the half cycle that started at last, 0 = unknown */
    uint8_t  good;        /* consecutive good half cycles, saturates */
    uint8_t  started;
    uint8_t  kind;        /* which hp[] the half cycle that started at last is */
    uint8_t  extra;       /* locked, an edge came 6..52 ticks after last: last may be noise */
    uint16_t hp[2];       /* the two kinds of half cycle, each smoothed */
    uint16_t undo;        /* hp[] of the half cycle that ended at last before it was learned, 0 = not learned */
} zc_tracker_t;

void zc_tracker_init(zc_tracker_t *z);
/** Feed an edge timestamp. Returns 1 if the edge is a zero cross to act on
 * (output allowed only once zc_locked()), 0 if it was ignored as noise.
 * Accepted half cycles are always ZC_HALF_MIN_TICKS..ZC_HALF_MAX_TICKS long. */
uint8_t zc_tracker_edge(zc_tracker_t *z, uint16_t now);
/** Locked onto mains: enough regular half cycles in a row. */
uint8_t zc_locked(const zc_tracker_t *z);
/** Called with the time now: drops the lock if edges stopped. Returns 1 when
 * the lock was dropped by this call. */
uint8_t zc_tracker_check(zc_tracker_t *z, uint16_t now);

#endif
