#include "dimmer.h"
#include "tuya.h"

#define DP_PER_CHANNEL   6
#define DP_POWER_ON      14

enum { DP_ON, DP_LEVEL, DP_MIN, DP_SWITCH_TYPE, DP_MAX, DP_COUNTDOWN };

static dimmer_channel_t g_ch[DIMMER_CHANNELS];
static uint8_t          g_power_on;   /* 0 off, 1 on, 2 last state */

void dimmer_init(void) {
    for (uint8_t i = 0; i < DIMMER_CHANNELS; i++) {
        /* Factory defaults of the stock firmware. */
        g_ch[i].on          = 0;
        g_ch[i].level       = DIMMER_LEVEL_MAX;
        g_ch[i].min         = 200;
        g_ch[i].max         = DIMMER_LEVEL_MAX;
        g_ch[i].switch_type = 0;
        g_ch[i].countdown   = 0;
    }
    g_power_on = 0;
}

const dimmer_channel_t *dimmer_channel(uint8_t ch) {
    return &g_ch[ch];
}

static uint16_t clamp_level(uint32_t v) {
    if (v < DIMMER_LEVEL_MIN) {
        return DIMMER_LEVEL_MIN;
    }
    if (v > DIMMER_LEVEL_MAX) {
        return DIMMER_LEVEL_MAX;
    }
    return (uint16_t)v;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Map a DP id to (channel, index); returns 0 if it is not a channel DP. */
static uint8_t channel_dp(uint8_t dpid, uint8_t *ch, uint8_t *idx) {
    if (dpid < 1 || dpid > DIMMER_CHANNELS * DP_PER_CHANNEL) {
        return 0;
    }
    *ch  = (uint8_t)((dpid - 1) / DP_PER_CHANNEL);
    *idx = (uint8_t)((dpid - 1) % DP_PER_CHANNEL);
    return 1;
}

uint8_t dimmer_write_dp(uint8_t dpid, uint8_t type, const uint8_t *value, uint16_t len) {
    uint8_t ch, idx;

    if (dpid == DP_POWER_ON) {
        if (type != TUYA_DP_TYPE_ENUM || len != 1 || value[0] > 2) {
            return 0;
        }
        g_power_on = value[0];
        return 1;
    }
    if (!channel_dp(dpid, &ch, &idx)) {
        return 0;
    }
    dimmer_channel_t *c = &g_ch[ch];

    switch (idx) {
    case DP_ON:
        if (type != TUYA_DP_TYPE_BOOL || len != 1) {
            return 0;
        }
        c->on = value[0] ? 1 : 0;
        return 1;

    case DP_SWITCH_TYPE:
        if (type != TUYA_DP_TYPE_ENUM || len != 1) {
            return 0;
        }
        c->switch_type = value[0];
        return 1;

    default:
        break;
    }

    if (type != TUYA_DP_TYPE_VALUE || len != 4) {
        return 0;
    }
    uint32_t v = be32(value);

    switch (idx) {
    case DP_LEVEL:
        /* Unlike the stock firmware, a level written while off is kept for
         * the next "on". */
        c->level = clamp_level(v);
        return 1;

    case DP_MIN:
        c->min = clamp_level(v);
        if (c->min > c->max) {
            c->max = c->min;
        }
        return 1;

    case DP_MAX:
        c->max = clamp_level(v);
        if (c->max < c->min) {
            c->min = c->max;
        }
        return 1;

    case DP_COUNTDOWN:
        c->countdown = v;
        return 1;

    default:
        return 0;
    }
}

void dimmer_report_dp(uint8_t dpid) {
    uint8_t ch, idx;

    if (dpid == DP_POWER_ON) {
        tuya_report_enum(dpid, g_power_on);
        return;
    }
    if (!channel_dp(dpid, &ch, &idx)) {
        return;
    }
    const dimmer_channel_t *c = &g_ch[ch];

    switch (idx) {
    case DP_ON:          tuya_report_bool(dpid, c->on);           break;
    case DP_LEVEL:       tuya_report_value(dpid, c->level);       break;
    case DP_MIN:         tuya_report_value(dpid, c->min);         break;
    case DP_SWITCH_TYPE: tuya_report_enum(dpid, c->switch_type);  break;
    case DP_MAX:         tuya_report_value(dpid, c->max);         break;
    case DP_COUNTDOWN:   tuya_report_value(dpid, c->countdown);   break;
    default:             break;
    }
}

void dimmer_report_all(void) {
    for (uint8_t ch = 0; ch < DIMMER_CHANNELS; ch++) {
        tuya_report_begin(1);
        for (uint8_t i = 0; i < DP_PER_CHANNEL; i++) {
            dimmer_report_dp((uint8_t)(ch * DP_PER_CHANNEL + i + 1));
        }
        tuya_report_send();
    }
    tuya_report_begin(1);
    dimmer_report_dp(DP_POWER_ON);
    tuya_report_send();
}
