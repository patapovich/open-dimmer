#include "input_core.h"

void input_init(input_t *in) {
    in->pressed  = 0;
    in->held     = 0;
    in->run      = 0;
    in->press_ms = 0;
}

uint8_t input_sample(input_t *in, uint8_t active) {
    active = active ? 1 : 0;

    if (in->pressed) {
        if (in->press_ms < 0xFFFF) {
            in->press_ms++;
        }
    }

    if (active == in->pressed) {
        in->run = 0;
    } else {
        in->run++;
        uint8_t need = in->pressed ? INPUT_RELEASE_MS : INPUT_PRESS_MS;
        if (in->run >= need) {
            in->run = 0;
            if (!in->pressed) {
                in->pressed  = 1;
                in->held     = 0;
                in->press_ms = INPUT_PRESS_MS;   /* counted from the first low */
                return INPUT_EV_PRESS;
            }
            in->pressed = 0;
            return in->held ? INPUT_EV_RELEASE_AFTER_HOLD : INPUT_EV_RELEASE;
        }
    }

    if (in->pressed && !in->held && in->run == 0 && in->press_ms >= INPUT_HOLD_MS) {
        in->held = 1;
        return INPUT_EV_HOLD;
    }
    return INPUT_EV_NONE;
}
