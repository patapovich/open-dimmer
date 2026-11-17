/* Hardware-free input logic, unit tested on the PC (tests/test_input_core.c).
 * Sampled every 1 ms.
 *
 * Fast on purpose (the stock firmware needs 100 ms of steady low before it
 * sees a press, and its hold dimming runs on after release):
 * - press: INPUT_PRESS_MS low samples in a row (rejects spikes, ~3 ms latency)
 * - release: INPUT_RELEASE_MS high samples in a row. Long enough to bridge
 *   the gaps if the input turns out to be a 50 Hz pulse train from an
 *   optocoupler, short enough not to be felt.
 * - hold: pressed for INPUT_HOLD_MS, reported once per press. */
#ifndef INPUT_CORE_H
#define INPUT_CORE_H

#include <stdint.h>

#define INPUT_PRESS_MS     3
#define INPUT_RELEASE_MS   25
#define INPUT_HOLD_MS      400

enum {
    INPUT_EV_NONE    = 0,
    INPUT_EV_PRESS   = 1,
    INPUT_EV_RELEASE = 2,   /* after a short press (no hold) */
    INPUT_EV_HOLD    = 3,
    INPUT_EV_RELEASE_AFTER_HOLD = 4,
};

typedef struct {
    uint8_t  pressed;     /* debounced state */
    uint8_t  held;        /* hold reported for this press */
    uint8_t  run;         /* samples in a row that disagree with the state */
    uint16_t press_ms;    /* time pressed so far */
} input_t;

void    input_init(input_t *in);
/** One 1 ms sample; active = contact closed (pin low). Returns an event. */
uint8_t input_sample(input_t *in, uint8_t active);

#endif
