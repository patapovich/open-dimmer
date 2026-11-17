/* Phase-angle (trailing edge) output: TIM1 drives the two gates, TIM16 is the
 * 20 us time base, PA4 EXTI gives the zero crosses.
 *
 * Built with SIM_ZC=1, a TIM16 compare interrupt makes a virtual 50 Hz zero
 * cross instead of PA4 (no mains needed), so the whole output path can be
 * checked on the bench: TIM1 registers over SWD, PA0/PA1 on a logic analyzer.
 *
 * Safety: output only while locked onto regular zero crosses; if an edge is
 * late the timer wraps into compare 0 (gates low) by itself. */
#ifndef PHASE_H
#define PHASE_H

#include <stdint.h>
#include "dim_core.h"

typedef struct {
    uint32_t zc_edges;      /* zero crosses acted on */
    uint32_t zc_ignored;    /* edges dropped as noise or after a gap */
    uint16_t half_period;   /* ticks of 20 us */
    uint8_t  locked;
    uint8_t  lock_losses;
    uint16_t current[2];    /* value applied, 0..1000 */
    uint16_t compare[2];    /* TIM1 compare (2 us ticks) loaded at the last zero cross */
} phase_debug_t;

extern volatile phase_debug_t g_phase_dbg;   /* read over SWD */

#if MEASURE || DIAG_REPORT
/* Measurement / diagnostics builds: durations of the PA4 levels between edges (20 us ticks). */
typedef struct {
    uint32_t sum;
    uint16_t count;
    uint16_t min;
    uint16_t max;
} zc_level_stats_t;

typedef struct {
    zc_level_stats_t high;
    zc_level_stats_t low;
    uint16_t         edges;
    uint16_t         glitches;   /* edges less than 2 ms after the previous one */
} zc_measure_t;

extern volatile zc_measure_t g_zc_measure;
#endif

void phase_init(void);
/** New target for a channel (0 = channel 1). Fades there at 5 per half cycle. */
void phase_set(uint8_t ch, uint8_t on, uint16_t level);
/** Physical output limits of a channel (levels 10..1000, see dim_output). */
void     phase_set_range(uint8_t ch, uint16_t min, uint16_t max);
/** Dimming curve of a channel (curves.h, 0 = linear like the stock firmware). */
void     phase_set_curve(uint8_t ch, uint8_t curve);
/** Hold dimming on a channel: the light follows directly between lo and hi. */
void     phase_hold_start(uint8_t ch, int8_t dir, uint16_t lo, uint16_t hi, uint8_t from_dark);
/** End of hold: frozen at the value the light has now, which is returned. */
uint16_t phase_hold_stop(uint8_t ch);
/** A provisional hold ends without a level (a tap from dark): fade quickly on. */
void     phase_hold_cancel(uint8_t ch);
/** How a channel fades and holds (dim_core.h). */
void     phase_set_profile(uint8_t ch, const dim_profile_t *p);
/** Value the channel is at now (0..1000). */
uint16_t phase_current(uint8_t ch);
/** Gates to plain GPIO low, for good (before a reset into the bootloader). */
void     phase_shutdown(void);
/** Main loop: notices lost zero crosses. */
void phase_poll(void);
/** 1 if a ~5 ms CPU stall (flash save) would not miss a zero cross now. */
uint8_t phase_save_window(void);

#endif
