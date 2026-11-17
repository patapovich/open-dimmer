#include "gesture_core.h"
#include "input_core.h"

void gesture_init(gesture_t *g) {
    g->count      = 0;
    g->down       = 0;
    g->pending    = 0;
    g->release_ms = 0;
}

uint8_t gesture_event(gesture_t *g, uint8_t input_ev, uint32_t now) {
    switch (input_ev) {
    case INPUT_EV_PRESS:
        if (!g->pending) {
            g->count = 0;
        }
        if (g->count < 0xFF) {
            g->count++;
        }
        g->down    = 1;
        g->pending = 1;
        return GESTURE_NONE;

    case INPUT_EV_RELEASE:
        g->down       = 0;
        g->release_ms = now;
        return GESTURE_NONE;

    case INPUT_EV_HOLD:
        g->pending = 0;
        g->count   = 0;
        return GESTURE_HOLD;

    case INPUT_EV_RELEASE_AFTER_HOLD:
        g->down    = 0;
        g->pending = 0;
        return GESTURE_RELEASE;

    default:
        return GESTURE_NONE;
    }
}

uint8_t gesture_poll(gesture_t *g, uint32_t now) {
    if (!g->pending || g->down || now - g->release_ms < GESTURE_GAP_MS) {
        return GESTURE_NONE;
    }
    g->pending = 0;
    return g->count <= GESTURE_TRIPLE ? g->count : GESTURE_NONE;
}
