/* Hardware-free press gestures, unit tested on the PC (tests/test_gesture_core.c).
 *
 * Fed with the debounced events of one input (input_core.h) and the time.
 * The light does not wait for any of this: it acts on each press as before,
 * and the gesture is reported on top of it for automations.
 *   single / double / triple: that many short presses, each started within
 *     GESTURE_GAP_MS of the previous release; reported GESTURE_GAP_MS after
 *     the last release (more than three: nothing, it was not meant).
 *   hold: the press became a hold (any short presses before it are dropped).
 *   release: the end of a hold. */
#ifndef GESTURE_CORE_H
#define GESTURE_CORE_H

#include <stdint.h>

#define GESTURE_GAP_MS     400

enum {
    GESTURE_NONE    = 0,
    GESTURE_SINGLE  = 1,
    GESTURE_DOUBLE  = 2,
    GESTURE_TRIPLE  = 3,
    GESTURE_HOLD    = 4,
    GESTURE_RELEASE = 5,
    GESTURE_PRESS   = 6,   /* fast press (DP 170 / 171, detached momentary): at once, no multi-taps */
};

typedef struct {
    uint8_t  count;       /* short presses in this sequence */
    uint8_t  down;        /* pressed now */
    uint8_t  pending;     /* a sequence waits for its gap to end */
    uint32_t release_ms;  /* time of the last release */
} gesture_t;

void    gesture_init(gesture_t *g);
/** An input event (INPUT_EV_*). Returns a gesture that ends now (hold, release). */
uint8_t gesture_event(gesture_t *g, uint8_t input_ev, uint32_t now);
/** Call often. Returns single/double/triple once the gap after the last release has passed. */
uint8_t gesture_poll(gesture_t *g, uint32_t now);

#endif
