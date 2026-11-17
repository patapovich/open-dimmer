/* Hardware-free part of the phase-control engine, unit tested on the PC
 * (tests/test_dim_core.c). Times are in ticks of 20 us (TIM16 / TIM1 with
 * PSC 479 at 24 MHz), so a 50 Hz half cycle is 500 ticks.
 *
 * Model taken from the stock firmware of the twin WiFi board: on every zero
 * cross each channel's current value moves towards its target by a fixed
 * step, and the output conducts from the zero cross for value/1000 of the half
 * cycle (trailing edge), linear in time. */
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

typedef struct {
    uint16_t current;   /* value applied now, 0..1000 */
    uint16_t target;    /* 0 when off */
    uint8_t  step;
} dim_fader_t;

void     dim_fader_init(dim_fader_t *f);
/** Target for this channel: 0 when off, else the level (10..1000). */
void     dim_fader_set(dim_fader_t *f, uint8_t on, uint16_t level);
/** One half cycle: move current towards target by step, return current. */
uint16_t dim_fader_tick(dim_fader_t *f);

/** Compare value for the conduction time of value/1000 of a half period. */
uint16_t dim_compare(uint16_t half_period, uint16_t value);
/** Auto-reload: a little past the expected next zero cross, so a late edge
 * does not start a new pulse mid cycle (the wrap loads compare 0 instead). */
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
