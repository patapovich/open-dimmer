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
#define LATE_PRESS_MS     10000   /* cold start: a long button hold begun by then is no rejoin */
#define HOLD_CAP_MS       2000    /* + 2 x hold ms: a hold has reached its end by then */
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
/* Switch type the press state above belongs to. */
static uint8_t g_type[DIMMER_CHANNELS];
/* Inputs closed at boot, until their release (bit per input): that press
 * started before us, so its end is no gesture and no button action. */
static uint8_t g_boot_held;
/* Cold start, the first LATE_PRESS_MS: a button hold that begins now may be a bootloader
 * rescue press that came too late, and the press now running began then. */
static uint8_t g_early;
static uint8_t g_button_early;

static uint32_t  g_now;   /* time of the events being handled (controls_poll) */

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
        /* A state type switch says what the light is. */
        if (g_type[ch] == SWITCH_STATE) {
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
            if (g_type[ch] == SWITCH_STATE) {
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
        }
    }
}

uint8_t controls_idle(void) {
    _Static_assert(IN_COUNT == 3, "every input");
    return !(g_in[0].pressed | g_in[1].pressed | g_in[2].pressed) && g_tail == g_head;
}
