#include "phase.h"
#include "board.h"
#include "dim_core.h"
#include "py32f0xx_ll_bus.h"
#include "py32f0xx_ll_exti.h"
#include "py32f0xx_ll_gpio.h"

#define TICK_PSC          479u   /* TIM16: 24 MHz / 480 = 50 kHz, 20 us */
#define OUT_PSC           47u    /* TIM1: 24 MHz / 48 = 500 kHz, 2 us */
#define SIM_HALF_PERIOD   500u   /* 50 Hz */
#define SAVE_WINDOW_TICKS 50u    /* 1 ms after a zero cross: 9 ms until the next */
#define ZC_PIN_MASK       (1u << 4)

volatile phase_debug_t g_phase_dbg;

#if MEASURE || DIAG_REPORT
volatile zc_measure_t g_zc_measure;
static uint16_t       g_meas_last;
static uint8_t        g_meas_started;

/* Every edge of PA4: how long the level before it lasted. */
static void measure_edge(uint16_t now) {
    uint8_t  high_now = LL_GPIO_IsInputPinSet(ZC_PORT, ZC_PIN) ? 1 : 0;
    uint16_t dt       = (uint16_t)(now - g_meas_last);
    g_meas_last = now;
    if (!g_meas_started) {
        g_meas_started = 1;
        return;
    }
    g_zc_measure.edges++;
    if (dt < 100u) {                         /* < 2 ms: a glitch, not a half cycle */
        g_zc_measure.glitches++;
        return;
    }
    zc_level_stats_t *s = high_now ? (zc_level_stats_t *)&g_zc_measure.low   /* it was low */
                                   : (zc_level_stats_t *)&g_zc_measure.high;
    s->sum += dt;
    s->count++;
    if (s->min == 0 || dt < s->min) {
        s->min = dt;
    }
    if (dt > s->max) {
        s->max = dt;
    }
}
#endif

static dim_fader_t       g_fader[2];
static volatile uint8_t  g_curve[2];
static volatile uint16_t g_min[2] = { 10, 10 }, g_max[2] = { 1000, 1000 };
static zc_tracker_t      g_zc;
static volatile uint16_t g_last_zc;

#if SIM_ZC
/* The bench build makes its own zero crosses. If the real detector moves, the
 * module is on the mains: never drive the gates out of step with it. */
static volatile uint8_t g_sim_blocked;
static uint8_t          g_sim_zc_level;
static uint8_t          g_sim_zc_changes;
#endif

/* Channel 1 is PA1 = TIM1_CH4, channel 2 is PA0 = TIM1_CH3 (stock wiring). */
static void zero_cross(uint16_t now) {
#if SIM_ZC
    if (g_sim_blocked) {
        return;
    }
#endif
    uint8_t was_locked = zc_locked(&g_zc);
    uint8_t accepted   = zc_tracker_edge(&g_zc, now);

    if (was_locked && !zc_locked(&g_zc)) {
        /* Lost the lock on a late edge: fade in again once relocked. */
        g_fader[0].current = 0;
        g_fader[1].current = 0;
        g_phase_dbg.lock_losses++;
    }
    if (!accepted) {
        g_phase_dbg.zc_ignored++;
        return;
    }
    g_last_zc = now;
    g_phase_dbg.zc_edges++;

    uint16_t hp = g_zc.half_period;
    uint16_t c0 = 0, c1 = 0;

    if (zc_locked(&g_zc)) {
        /* Fade only while the output runs, so the first half cycles after
         * power-up fade in instead of jumping to the target. */
        dim_fader_set_half_period(&g_fader[0], hp);
        dim_fader_set_half_period(&g_fader[1], hp);
        c0 = dim_compare(hp, dim_output(g_curve[0], dim_fader_tick(&g_fader[0]), g_min[0], g_max[0]));
        c1 = dim_compare(hp, dim_output(g_curve[1], dim_fader_tick(&g_fader[1]), g_min[1], g_max[1]));
    }

    TIM1->ARR  = dim_reload(hp);
    TIM1->CCR4 = c0;
    TIM1->CCR3 = c1;
    TIM1->EGR  = TIM_EGR_UG;      /* counter to 0, ARR/CCR preloads to active */
    /* Arm the safe default: if the next edge does not come before the wrap,
     * the wrap's update event loads compare 0 and the gates stay low. */
    TIM1->CCR4 = 0;
    TIM1->CCR3 = 0;

    g_phase_dbg.half_period = hp;
    g_phase_dbg.locked      = zc_locked(&g_zc);
    g_phase_dbg.current[0]  = g_fader[0].current;
    g_phase_dbg.current[1]  = g_fader[1].current;
    g_phase_dbg.compare[0]  = c0;
    g_phase_dbg.compare[1]  = c1;
}

#if SIM_ZC
void TIM16_IRQHandler(void) {
    if (TIM16->SR & TIM_SR_CC1IF) {
        TIM16->SR = ~TIM_SR_CC1IF;
        uint16_t t   = (uint16_t)TIM16->CCR1;
        TIM16->CCR1 = (uint16_t)(t + SIM_HALF_PERIOD);
        zero_cross(t);
    }
}
#else
void EXTI4_15_IRQHandler(void) {
    if (EXTI->PR & ZC_PIN_MASK) {
        EXTI->PR = ZC_PIN_MASK;
        uint16_t now = (uint16_t)TIM16->CNT;
#if DIAG_REPORT
        /* The LED (active low) follows PA4 at once: a phototransistor on it and
         * a mains transformer on an isolated scope give the detector's delay. */
        if (LL_GPIO_IsInputPinSet(ZC_PORT, ZC_PIN)) {
            LL_GPIO_ResetOutputPin(LED_PORT, LED_PIN);
        } else {
            LL_GPIO_SetOutputPin(LED_PORT, LED_PIN);
        }
#endif
#if MEASURE || DIAG_REPORT
        measure_edge(now);
#endif
        zero_cross(now);
    }
}
#endif

static void timers_init(void) {
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_TIM1 | LL_APB1_GRP2_PERIPH_TIM16);

    /* TIM16: free-running 20 us time base. */
    TIM16->PSC = TICK_PSC;
    TIM16->ARR = 0xFFFF;
    TIM16->EGR = TIM_EGR_UG;
    TIM16->SR  = 0;
#if SIM_ZC
    TIM16->CCR1  = SIM_HALF_PERIOD;
    TIM16->DIER |= TIM_DIER_CC1IE;
    NVIC_SetPriority(TIM16_IRQn, 0);
    NVIC_EnableIRQ(TIM16_IRQn);
#endif
    TIM16->CR1 = TIM_CR1_CEN;

    /* TIM1: PWM mode 1 on CH3/CH4, high from the zero cross until compare. */
    TIM1->PSC   = OUT_PSC;
    TIM1->ARR   = 0xFFFF;
    TIM1->CCR3  = 0;
    TIM1->CCR4  = 0;
    TIM1->CCMR2 = (6u << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE |
                  (6u << TIM_CCMR2_OC4M_Pos) | TIM_CCMR2_OC4PE;
    TIM1->CCER  = TIM_CCER_CC3E | TIM_CCER_CC4E;   /* active high */
    TIM1->BDTR  = TIM_BDTR_MOE;
    TIM1->CR1   = TIM_CR1_ARPE;
    TIM1->EGR   = TIM_EGR_UG;
    TIM1->CR1  |= TIM_CR1_CEN;
}

#if !MEASURE
static void gate_pins_to_timer(void) {
    /* The pins are GPIO outputs driven low until here; the timer outputs
     * are low too (compare 0), so the switch over does not glitch. */
    LL_GPIO_SetAFPin_0_7(GATE1_PORT, GATE1_PIN, LL_GPIO_AF_13);
    LL_GPIO_SetAFPin_0_7(GATE2_PORT, GATE2_PIN, LL_GPIO_AF_13);
    LL_GPIO_SetPinMode(GATE1_PORT, GATE1_PIN, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinMode(GATE2_PORT, GATE2_PIN, LL_GPIO_MODE_ALTERNATE);
}
#endif

static void zero_cross_input_init(void) {
#if !SIM_ZC
    LL_GPIO_SetPinMode(ZC_PORT, ZC_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(ZC_PORT, ZC_PIN, LL_GPIO_PULL_UP);
    LL_EXTI_SetEXTISource(LL_EXTI_CONFIG_PORTA, LL_EXTI_CONFIG_LINE4);
    EXTI->RTSR |= ZC_PIN_MASK;
    EXTI->FTSR |= ZC_PIN_MASK;
    EXTI->PR    = ZC_PIN_MASK;
    EXTI->IMR  |= ZC_PIN_MASK;
    NVIC_SetPriority(EXTI4_15_IRQn, 0);
    NVIC_EnableIRQ(EXTI4_15_IRQn);
#endif
}

void phase_init(void) {
    dim_fader_init(&g_fader[0]);
    dim_fader_init(&g_fader[1]);
    zc_tracker_init(&g_zc);
    timers_init();
#if !MEASURE
    gate_pins_to_timer();       /* measurement build: the gates stay GPIO low, always */
#endif
    zero_cross_input_init();
}

void phase_set(uint8_t ch, uint8_t on, uint16_t level) {
    if (ch < 2) {
        dim_fader_set(&g_fader[ch], on, level);   /* 16-bit store: atomic */
    }
}

void phase_set_range(uint8_t ch, uint16_t min, uint16_t max) {
    if (ch < 2) {
        __disable_irq();
        g_min[ch] = min;
        g_max[ch] = max;
        dim_fader_set_floor(&g_fader[ch], min);
        __enable_irq();
    }
}

void phase_set_curve(uint8_t ch, uint8_t curve) {
    if (ch < 2) {
        g_curve[ch] = curve;
    }
}

void phase_hold_start(uint8_t ch, int8_t dir, uint16_t lo, uint16_t hi, uint8_t from_dark) {
    if (ch >= 2) {
        return;
    }
    __disable_irq();
    if (from_dark) {
        dim_fader_hold_start_from_dark(&g_fader[ch], lo, hi);
    } else {
        dim_fader_hold_start(&g_fader[ch], dir, lo, hi);
    }
    __enable_irq();
}

uint16_t phase_hold_stop(uint8_t ch) {
    if (ch >= 2) {
        return 0;
    }
    __disable_irq();
    uint16_t v = dim_fader_hold_stop(&g_fader[ch]);
    __enable_irq();
    return v;
}

void phase_set_profile(uint8_t ch, const dim_profile_t *p) {
    if (ch < 2) {
        __disable_irq();
        dim_fader_set_profile(&g_fader[ch], p);
        __enable_irq();
    }
}

void phase_hold_cancel(uint8_t ch) {
    if (ch < 2) {
        __disable_irq();
        dim_fader_hold_cancel(&g_fader[ch]);
        __enable_irq();
    }
}

uint16_t phase_current(uint8_t ch) {
    return ch < 2 ? g_fader[ch].current : 0;
}

void phase_shutdown(void) {
    __disable_irq();
    LL_GPIO_ResetOutputPin(GATE1_PORT, GATE1_PIN);
    LL_GPIO_ResetOutputPin(GATE2_PORT, GATE2_PIN);
    LL_GPIO_SetPinMode(GATE1_PORT, GATE1_PIN, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetPinMode(GATE2_PORT, GATE2_PIN, LL_GPIO_MODE_OUTPUT);
    TIM1->BDTR &= ~TIM_BDTR_MOE;
    __enable_irq();
}

void phase_poll(void) {
#if SIM_ZC
    uint8_t level = LL_GPIO_IsInputPinSet(ZC_PORT, ZC_PIN) ? 1 : 0;
    if (level != g_sim_zc_level) {
        g_sim_zc_level = level;
        if (++g_sim_zc_changes >= 4 && !g_sim_blocked) {
            g_sim_blocked = 1;
            phase_shutdown();
        }
    }
#endif
    __disable_irq();
    uint8_t lost = zc_tracker_check(&g_zc, (uint16_t)TIM16->CNT);
    if (lost) {
        /* Mains gone or the detector stopped: start from dark next time. */
        g_fader[0].current = 0;
        g_fader[1].current = 0;
        g_phase_dbg.locked = 0;
        g_phase_dbg.lock_losses++;
    }
    __enable_irq();
}

uint8_t phase_save_window(void) {
    if (!zc_locked(&g_zc)) {
        return g_zc.good == 0;      /* no mains at all: any time; locking: not now */
    }
    return (uint16_t)((uint16_t)TIM16->CNT - g_last_zc) < SAVE_WINDOW_TICKS;
}
