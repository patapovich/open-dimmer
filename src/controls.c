#include "controls.h"
#include "board.h"
#include "dimmer.h"
#include "input_core.h"
#include "gesture_core.h"
#include "tuya.h"
#include "py32f0xx_ll_utils.h"
#include "dim_core.h"

_Static_assert(DIM_HOLD_DARK_WAIT_MS >= INPUT_HOLD_MS + 200u,
               "a hold from dark waits at the min past the hold time, so it can be let go there");

#define IN_S1             0
#define IN_S2             1
#define IN_BUTTON         2
#define IN_COUNT          3

#define SWITCH_TOGGLE     0
#define SWITCH_STATE      1
#define SWITCH_MOMENTARY  2

#define BUTTON_RESET_MS   3000
#define LATE_PRESS_MS     10000   /* cold start: a long button hold begun by then is no rejoin */
#define HOLD_CAP_MS       2000    /* + 2 x hold ms: a hold has reached its end by then */
#define EV_RING           16      /* power of two */
#define RESET_WINDOW_MS   10000   /* the presses of a network reset: within this of the first */
#define RESET_BLINK_S     3       /* ... then the switch's channel blinks that long */

volatile uint8_t g_input_override;

static input_t           g_in[IN_COUNT];
static volatile uint8_t  g_ring[EV_RING];
static volatile uint8_t  g_head, g_tail;
static volatile uint16_t g_button_press_ms;   /* duration of the last button press */

/* Momentary switches: did this press switch the light on? */
static uint8_t g_turned_on_by_press[DIMMER_CHANNELS];
/* Gestures of the wall switches (momentary type), reported for automations. */
static gesture_t g_gesture[DIMMER_CHANNELS];
static uint32_t  g_tap_at[DIMMER_CHANNELS];    /* a tap's release (| 1), 0: none: a double tap */
static uint8_t   g_double[DIMMER_CHANNELS];    /* this press began within the gap of a tap's */
/* Switch type the press state above belongs to. */
static uint8_t g_type[DIMMER_CHANNELS];
/* The wall switch mode each channel was handled in (DP 162 / 163 change it under a press), and
 * a momentary press that began in another mode: its release (and hold) is not acted on. */
static uint8_t g_mode[DIMMER_CHANNELS];
static uint8_t g_skip_release;
/* Inputs closed at boot, until their release (bit per input): that press
 * started before us, so its end is no gesture and no button action. */
static uint8_t g_boot_held;
/* Cold start, the first LATE_PRESS_MS: a button hold that begins now may be a bootloader
 * rescue press that came too late, and the press now running began then. */
static uint8_t g_early;
static uint8_t g_button_early;

static uint32_t  g_now;   /* time of the events being handled (controls_poll) */

#if DIMMER_RESET_BY_PRESSES
/* Network reset by presses (DP 181), as the stock MCU counts them: closures of one wall switch,
 * the first opens the window; another switch's closure starts a new count. */
static uint8_t  g_reset_input;
static uint8_t  g_reset_count;
static uint32_t g_reset_start;
#endif

static uint8_t closed(uint8_t idx);


void controls_init(uint8_t cold_start) {
    g_early = cold_start;
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
            g_boot_held    |= (uint8_t)(1u << i);
        }
    }
    __enable_irq();
    for (uint8_t ch = 0; ch < DIMMER_CHANNELS; ch++) {
        gesture_init(&g_gesture[ch]);
        g_type[ch] = dimmer_channel(ch)->switch_type;
        g_mode[ch] = dimmer_channel(ch)->input_mode;
        /* A state type switch says what the light is (unless detached or off). */
        if (g_type[ch] == SWITCH_STATE && dimmer_channel(ch)->input_mode == DIMMER_INPUT_NORMAL) {
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
        uint8_t ev = input_sample(&g_in[i], closed(i));
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

    if (g_skip_release & (1u << ch)) {
        /* A momentary press that began in the other mode: its hold and release are not this
         * mode's (a new press is). */
        if (ev == INPUT_EV_PRESS || c->switch_type != SWITCH_MOMENTARY) {
            g_skip_release &= (uint8_t)~(1u << ch);
        } else {
            if (ev != INPUT_EV_HOLD) {
                g_skip_release &= (uint8_t)~(1u << ch);
            }
            return;
        }
    }

    if (c->input_mode != DIMMER_INPUT_NORMAL) {
        /* Detached: the light is the network's; the switch only reports (a momentary its
         * gestures, a toggle or state switch each change as a single). Off: ignored. */
        if (c->input_mode == DIMMER_INPUT_DETACHED) {
            uint8_t g;
            if (c->switch_type != SWITCH_MOMENTARY) {
                g = ev == INPUT_EV_PRESS || ev == INPUT_EV_RELEASE ||
                    ev == INPUT_EV_RELEASE_AFTER_HOLD ? GESTURE_SINGLE : 0;
            } else if (c->fast_press) {
                /* Direct control: the press at once, a hold and its release; no multi-taps. */
                g = ev == INPUT_EV_PRESS ? GESTURE_PRESS : ev == INPUT_EV_HOLD ? GESTURE_HOLD
                  : ev == INPUT_EV_RELEASE_AFTER_HOLD ? GESTURE_RELEASE : 0;
            } else {
                g = gesture_event(&g_gesture[ch], ev, g_now);
            }
            dimmer_report_action(ch, g);
        }
        return;
    }
    switch (c->switch_type) {
    case SWITCH_STATE:
        if (ev == INPUT_EV_PRESS) {
            dimmer_local_set(ch, 1);
        } else if (ev == INPUT_EV_RELEASE || ev == INPUT_EV_RELEASE_AFTER_HOLD) {
            dimmer_local_set(ch, 0);
        }
        break;

    case SWITCH_MOMENTARY:
        switch (ev) {
        case INPUT_EV_PRESS:
            /* From dark: on at once at the minimum, rising slowly while the
             * finger stays (a hold picks a level from the bottom, with no
             * flash to the stored level); a tap then goes quickly on to the
             * stored level. A press within GESTURE_GAP_MS of a tap's release
             * that is a tap too (decided at its release, so a tap and a hold
             * still dims): the light to full, from either state. */
            g_double[ch] = !c->double_off && g_tap_at[ch] && g_now - g_tap_at[ch] <= GESTURE_GAP_MS;
            g_tap_at[ch] = 0;
            g_turned_on_by_press[ch] = !c->on;
            if (!c->on) {
                dimmer_local_set(ch, 1);
                dimmer_hold_begin(ch, +1, 1);
            }
            break;
        case INPUT_EV_RELEASE:
            if (g_double[ch]) {
                dimmer_local_full(ch);          /* a third tap is a plain one again */
            } else {
                g_tap_at[ch] = g_now | 1u;
                if (g_turned_on_by_press[ch]) {
                    dimmer_hold_cancel(ch);
                } else {
                    dimmer_local_set(ch, 0);
                }
            }
            break;
        case INPUT_EV_HOLD:
            if (!g_turned_on_by_press[ch]) {
                dimmer_hold_begin(ch, 0, 0);
            } else if (!dimmer_holding(ch)) {   /* from dark the rise is already running, unless */
                dimmer_hold_begin(ch, 0, 2);    /* the channel has it off: a hold from its level */
            } else {
                dimmer_hold_reports(ch);
            }
            break;
        case INPUT_EV_RELEASE_AFTER_HOLD:
            dimmer_hold_end(ch);
            break;
        default:
            break;
        }
        /* The light acts first, the gesture is reported on top of it (after a
         * hold, 'release' follows the report of the new level). */
        dimmer_report_action(ch, gesture_event(&g_gesture[ch], ev, g_now));
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

#if DIMMER_RESET_BY_PRESSES
/* A wall switch closed (its press, any switch type): one more of a network reset's presses. */
static void reset_press(uint8_t ch) {
    uint8_t need = dimmer_reset_presses();
    if (!need || dimmer_channel(ch)->input_mode == DIMMER_INPUT_OFF) {
        g_reset_count = 0;
        return;
    }
    if (!g_reset_count || ch != g_reset_input || g_now - g_reset_start >= RESET_WINDOW_MS) {
        g_reset_count = 0;
        g_reset_input = ch;
        g_reset_start = g_now;
    }
    if (++g_reset_count >= need) {
        g_reset_count = 0;
        uint8_t rejoin = TUYA_RESET_NETWORK_REJOIN;
        tuya_send(TUYA_CMD_RESET_NETWORK, 0, &rejoin, 1);
        dimmer_identify(ch, RESET_BLINK_S);
    }
}
#endif

/* Module button: both channels together (any on -> all off, else all on);
 * held 3 s: rejoin, unless the press began in the first 10 s after a cold start. */
static void button(uint8_t ev) {
    if (ev == INPUT_EV_PRESS) {
        g_button_early = g_early;
    } else if (ev == INPUT_EV_RELEASE) {
        dimmer_local_toggle_all();
    } else if (ev == INPUT_EV_RELEASE_AFTER_HOLD) {
        if (g_button_press_ms >= BUTTON_RESET_MS) {
            if (!g_button_early) {
                uint8_t rejoin = TUYA_RESET_NETWORK_REJOIN;
                tuya_send(TUYA_CMD_RESET_NETWORK, 0, &rejoin, 1);
            }
        } else {
            dimmer_local_toggle_all();
        }
    }
}

void controls_poll(uint32_t now) {
    g_now = now;
    if (now >= LATE_PRESS_MS) {
        g_early = 0;                  /* for good: the ms counter's wrap does not reopen it */
    }
    for (uint8_t ch = 0; ch < DIMMER_CHANNELS; ch++) {
        const dimmer_channel_t *c = dimmer_channel(ch);
        if (c->switch_type != g_type[ch]) {
            /* New switch type (DP 4 / 10): no press state of the old one is
             * kept, and a state type switch says what the light is. Here, not
             * in the DP write, whose ack is being built. */
            g_type[ch] = c->switch_type;
            gesture_init(&g_gesture[ch]);
            g_turned_on_by_press[ch] = 0;
            if (g_type[ch] == SWITCH_STATE && c->input_mode == DIMMER_INPUT_NORMAL) {
                dimmer_local_set(ch, g_in[ch].pressed);
            }
        }
        if (c->input_mode != g_mode[ch]) {
            /* New wall switch mode: no gesture of the old one is finished, a press under way
             * is not acted on when let go, and back in normal a state switch says the light. */
            g_mode[ch] = c->input_mode;
            gesture_init(&g_gesture[ch]);
            g_turned_on_by_press[ch] = 0;
            if (g_in[ch].pressed && g_type[ch] == SWITCH_MOMENTARY) {
                g_skip_release |= (uint8_t)(1u << ch);
            } else {
                g_skip_release &= (uint8_t)~(1u << ch);
            }
            if (g_type[ch] == SWITCH_STATE && c->input_mode == DIMMER_INPUT_NORMAL) {
                dimmer_local_set(ch, g_in[ch].pressed);
            }
        }
        /* A hold ends by itself once it has had time to reach its end: a
         * shorted or stuck momentary input would else keep the network's
         * levels out for good (nothing if no hold is running). */
        if (g_in[ch].press_ms > 2u * c->hold_ms + HOLD_CAP_MS) {
            dimmer_hold_end(ch);
        }
        dimmer_report_action(ch, gesture_poll(&g_gesture[ch], now));
    }
    while (g_tail != g_head) {
        uint8_t e = g_ring[g_tail];
        g_tail    = (uint8_t)((g_tail + 1) & (EV_RING - 1));

        uint8_t idx = e >> 4, ev = e & 0x0F;
        uint8_t bit = (uint8_t)(1u << idx);
        if (g_boot_held & bit) {
            /* The release of a press that started before boot (its first
             * event): for a toggle or state switch a real change, else
             * nothing (no 'release', no button toggle, no rejoin). */
            g_boot_held &= (uint8_t)~bit;
            if (idx == IN_BUTTON || g_type[idx] == SWITCH_MOMENTARY) {
                continue;
            }
        }
        if (idx == IN_BUTTON) {
            button(ev);
        } else {
            wall_switch(idx, ev);   /* S1 -> channel 1, S2 -> channel 2 */
#if DIMMER_RESET_BY_PRESSES
            if (ev == INPUT_EV_PRESS) {
                reset_press(idx);
            }
#endif
        }
    }
}

uint8_t controls_idle(void) {
    uint8_t busy = g_tail != g_head;              /* an event the main loop has not handled */
    for (uint8_t i = 0; i < IN_COUNT; i++) {
        busy |= g_in[i].run;                      /* a change being debounced: a press under way */
        /* Closed: a press of the button or a momentary switch under way, or a state switch that
         * would light its channel at the restart. A toggle switch resting closed loses nothing:
         * controls_init takes it as where it rests, and its next change toggles. */
        if (i == IN_BUTTON || g_type[i] != SWITCH_TOGGLE) {
            busy |= g_in[i].pressed;
        }
    }
    return !busy;
}
