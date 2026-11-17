#include "controls.h"
#include "board.h"
#include "dimmer.h"
#include "input_core.h"
#include "gesture_core.h"
#include "tuya.h"
#include "py32f0xx_ll_utils.h"

#define IN_S1             0
#define IN_S2             1
#define IN_BUTTON         2
#define IN_COUNT          3

#define SWITCH_TOGGLE     0
#define SWITCH_STATE      1
#define SWITCH_MOMENTARY  2

#define BUTTON_RESET_MS   3000
#define EV_RING           16      /* power of two */

volatile uint8_t g_input_override;

static input_t           g_in[IN_COUNT];
static volatile uint8_t  g_ring[EV_RING];
static volatile uint8_t  g_head, g_tail;
static volatile uint16_t g_button_press_ms;   /* duration of the last button press */

/* Momentary switches: did this press switch the light on? */
static uint8_t g_turned_on_by_press[DIMMER_CHANNELS];
/* Gestures of the wall switches (momentary type), reported for automations. */
static gesture_t g_gesture[DIMMER_CHANNELS];

static uint32_t  g_now;   /* time of the events being handled (controls_poll) */

static uint8_t closed(uint8_t idx);

#if !(MEASURE || DIAG_REPORT)   /* the measurement builds have their own input statistics */
/* Raw (undebounced) wall switch timing, to learn what the board's input stage
 * does: the last 8 level runs per switch, byte = closed << 7 | ms / 4 (max
 * 127 = 508 ms), oldest first. Reported after each gesture as DPs 140/141
 * (S1) and 142/143 (S2), 4 runs each, the run still going not included. */
#define RAW_RUNS          8
static uint8_t           g_raw_level[DIMMER_CHANNELS];
static uint16_t          g_raw_run_ms[DIMMER_CHANNELS];
static volatile uint8_t  g_raw_runs[DIMMER_CHANNELS][RAW_RUNS];
static volatile uint8_t  g_raw_head[DIMMER_CHANNELS];

static void raw_sample(uint8_t ch, uint8_t level) {
    if (level == g_raw_level[ch]) {
        if (g_raw_run_ms[ch] < 0xFFFF) {
            g_raw_run_ms[ch]++;
        }
        return;
    }
    uint16_t d = (uint16_t)(g_raw_run_ms[ch] / 4u);
    g_raw_runs[ch][g_raw_head[ch] & (RAW_RUNS - 1)] =
        (uint8_t)((d > 127u ? 127u : d) | (uint8_t)(g_raw_level[ch] << 7));
    g_raw_head[ch]++;
    g_raw_level[ch]  = level;
    g_raw_run_ms[ch] = 1;
}

static void report_raw_timing(uint8_t ch) {
    uint8_t r[RAW_RUNS];
    __disable_irq();
    uint8_t head = g_raw_head[ch];
    for (uint8_t k = 0; k < RAW_RUNS; k++) {
        r[k] = g_raw_runs[ch][(uint8_t)(head + k) & (RAW_RUNS - 1)];   /* oldest first */
    }
    __enable_irq();
    tuya_report_begin(0);
    tuya_report_value((uint8_t)(140 + 2 * ch),
                      ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3]);
    tuya_report_value((uint8_t)(141 + 2 * ch),
                      ((uint32_t)r[4] << 24) | ((uint32_t)r[5] << 16) | ((uint32_t)r[6] << 8) | r[7]);
    tuya_report_send();
}
#else
static void raw_sample(uint8_t ch, uint8_t level) {
    (void)ch;
    (void)level;
}

static void report_raw_timing(uint8_t ch) {
    (void)ch;
}
#endif

void controls_init(void) {
    /* Pull-ups and the input filters settle before the resting state is read
     * (the tick interrupt is not running yet, so nothing is sampled). */
    LL_mDelay(30);
    __disable_irq();
    g_head = 0;
    g_tail = 0;
    for (uint8_t i = 0; i < IN_COUNT; i++) {
        input_init(&g_in[i]);
        /* A switch already closed at power-up is where it rests, not a
         * press: take it as the starting state, with no event. */
        if (closed(i)) {
            g_in[i].pressed = 1;
            g_in[i].held    = 1;
        }
#if !(MEASURE || DIAG_REPORT)
        if (i < DIMMER_CHANNELS) {
            g_raw_level[i] = closed(i);
        }
#endif
    }
    __enable_irq();
    for (uint8_t ch = 0; ch < DIMMER_CHANNELS; ch++) {
        gesture_init(&g_gesture[ch]);
    }
    /* A state type switch says what the light is. */
    for (uint8_t ch = 0; ch < DIMMER_CHANNELS; ch++) {
        if (dimmer_channel(ch)->switch_type == SWITCH_STATE) {
            dimmer_boot_set(ch, g_in[ch].pressed);
        }
    }
}

static uint8_t closed(uint8_t idx) {
    uint8_t ov = g_input_override;
    if (ov & 0x80) {
        return (ov >> idx) & 1;
    }
    switch (idx) {
    case IN_S1:     return !LL_GPIO_IsInputPinSet(S1_PORT, S1_PIN);
    case IN_S2:     return !LL_GPIO_IsInputPinSet(S2_PORT, S2_PIN);
    default:        return !LL_GPIO_IsInputPinSet(BUTTON_PORT, BUTTON_PIN);
    }
}

void controls_tick_1ms(void) {
    for (uint8_t i = 0; i < IN_COUNT; i++) {
        uint8_t level = closed(i);
        if (i < DIMMER_CHANNELS) {
            raw_sample(i, level);
        }
        uint8_t ev = input_sample(&g_in[i], level);
        if (ev == INPUT_EV_NONE) {
            continue;
        }
        if (i == IN_BUTTON) {
            g_button_press_ms = g_in[i].press_ms;
        }
        uint8_t next = (uint8_t)((g_head + 1) & (EV_RING - 1));
        if (next != g_tail) {                 /* full: drop (cannot happen at human speed) */
            g_ring[g_head] = (uint8_t)((i << 4) | ev);
            g_head         = next;
        }
    }
}

static void wall_switch(uint8_t ch, uint8_t ev) {
    const dimmer_channel_t *c = dimmer_channel(ch);

    switch (c->switch_type) {
    case SWITCH_STATE:
        if (ev == INPUT_EV_PRESS) {
            dimmer_local_set(ch, 1);
        } else if (ev == INPUT_EV_RELEASE || ev == INPUT_EV_RELEASE_AFTER_HOLD) {
            dimmer_local_set(ch, 0);
        }
        break;

    case SWITCH_MOMENTARY:
        /* The light acts first, the gesture is reported on top of it. */
        {
            uint8_t g = gesture_event(&g_gesture[ch], ev, g_now);
            if (g) {
                dimmer_report_action(ch, g);
                if (g == GESTURE_RELEASE) {
                    report_raw_timing(ch);
                }
            }
        }
        switch (ev) {
        case INPUT_EV_PRESS:
            /* From dark: on at once at the minimum, rising slowly while the
             * finger stays (a hold picks a level from the bottom, with no
             * flash to the stored level); a tap then goes quickly on to the
             * stored level. */
            g_turned_on_by_press[ch] = !c->on;
            if (!c->on) {
                dimmer_local_set(ch, 1);
                dimmer_hold_begin(ch, +1, 1);
            }
            break;
        case INPUT_EV_RELEASE:
            if (g_turned_on_by_press[ch]) {
                dimmer_hold_cancel(ch);
            } else {
                dimmer_local_set(ch, 0);
            }
            break;
        case INPUT_EV_HOLD:
            if (!g_turned_on_by_press[ch]) {
                dimmer_hold_begin(ch, 0, 0);    /* from dark the rise is already running */
            }
            break;
        case INPUT_EV_RELEASE_AFTER_HOLD:
            dimmer_hold_end(ch);
            break;
        default:
            break;
        }
        break;

    case SWITCH_TOGGLE:
    default:
        if (ev == INPUT_EV_PRESS || ev == INPUT_EV_RELEASE ||
            ev == INPUT_EV_RELEASE_AFTER_HOLD) {
            dimmer_local_toggle(ch);
        }
        break;
    }
}

/* Module button: both channels together (any on -> all off, else all on);
 * held 3 s: rejoin. */
static void button(uint8_t ev) {
    if (ev == INPUT_EV_RELEASE) {
        dimmer_local_toggle_all();
    } else if (ev == INPUT_EV_RELEASE_AFTER_HOLD) {
        if (g_button_press_ms >= BUTTON_RESET_MS) {
            uint8_t rejoin = TUYA_RESET_NETWORK_REJOIN;
            tuya_send(TUYA_CMD_RESET_NETWORK, 0, &rejoin, 1);
        } else {
            dimmer_local_toggle_all();
        }
    }
}

void controls_poll(uint32_t now) {
    g_now = now;
    while (g_tail != g_head) {
        uint8_t e = g_ring[g_tail];
        g_tail    = (uint8_t)((g_tail + 1) & (EV_RING - 1));

        uint8_t idx = e >> 4, ev = e & 0x0F;
        if (idx == IN_BUTTON) {
            button(ev);
        } else {
            wall_switch(idx, ev);   /* S1 -> channel 1, S2 -> channel 2 */
        }
    }
    for (uint8_t ch = 0; ch < DIMMER_CHANNELS; ch++) {
        uint8_t g = gesture_poll(&g_gesture[ch], now);
        if (g) {
            dimmer_report_action(ch, g);
            report_raw_timing(ch);
        }
    }
}
