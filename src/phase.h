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

typedef struct {
    uint32_t zc_edges;      /* zero crosses acted on */
    uint32_t zc_ignored;    /* edges dropped as noise or after a gap */
    uint16_t half_period;   /* ticks of 20 us */
    uint8_t  locked;
    uint8_t  lock_losses;
    uint16_t current[2];    /* value applied, 0..1000 */
    uint16_t compare[2];    /* TIM1 compare loaded at the last zero cross */
} phase_debug_t;

extern volatile phase_debug_t g_phase_dbg;   /* read over SWD */

void phase_init(void);
/** New target for a channel (0 = channel 1). Fades there at 5 per half cycle. */
void phase_set(uint8_t ch, uint8_t on, uint16_t level);
/** Main loop: notices lost zero crosses. */
void phase_poll(void);
/** 1 if a ~5 ms CPU stall (flash save) would not miss a zero cross now. */
uint8_t phase_save_window(void);

#endif
