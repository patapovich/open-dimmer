#include "dimmer.h"
#include "phase.h"
#include "store.h"
#include "curves.h"
#include "dim_core.h"
#include "ota_proto.h"
#include "tuya.h"
#include <string.h>

#define DP_PER_CHANNEL   6
#define DP_POWER_ON      14    /* both channels (stock); reported: channel 1's */
#define DP_CURVE_BASE    101   /* + channel */
#define DP_POWER_ON_BASE 103   /* + channel: power-on behaviour of one channel (ours) */
#define DP_ACTION_BASE   105   /* + channel: wall switch gesture, event only (ours) */
#define DP_PROFILE_BASE  150   /* + 5 * channel: fade on ms, fade off ms, soft end %, */
#define DP_PROFILE_COUNT 5     /* hold ms, hold end % (ours, saved)                  */
enum { PF_FADE_ON, PF_FADE_OFF, PF_SOFT_END, PF_HOLD, PF_HOLD_END };

enum { DP_ON, DP_LEVEL, DP_MIN, DP_SWITCH_TYPE, DP_MAX, DP_COUNTDOWN };

#define POWER_ON_OFF     0
#define POWER_ON_ON      1
#define POWER_ON_LAST    2

#define SAVE_DELAY_MS    2000   /* coalesce bursts of writes into one save */
#define SAVE_QUIET_MS    100    /* the CPU stalls ~5 ms while saving: not mid-frame */
#define SAVED_FORMAT     3     /* 1: one power-on for both channels; 2: no fade profile */
#define SAVE_RETRIES     3
#define RESTORE_CLEAR_MS 60000  /* a run this long ends a reset loop: the next reset restores again */

static dimmer_channel_t g_ch[DIMMER_CHANNELS];
static uint8_t          g_power_on[DIMMER_CHANNELS];   /* 0 off, 1 on, 2 last state */

/* What survives a power cut. The level is the one set while off, the one a
 * wall switch hold picked, or the one the light had when it was switched off:
 * a level change from the network while on (adaptive lighting every few
 * minutes) is not saved by itself, to spare the flash. The on state is kept
 * only for "last state" power-on. */
typedef struct {
    uint8_t format;
    uint8_t power_on;     /* channel 1 (format 1: both) */
    uint8_t on_mask;
    uint8_t power_on_2;   /* channel 2 (format 2; was reserved = 0) */
    struct {
        uint16_t level;
        uint16_t min;
        uint16_t max;
        uint8_t  switch_type;
        uint8_t  curve;      /* was reserved (0) in early records: 0 = linear */
    } ch[DIMMER_CHANNELS];
    /* Format 3. Stored as value + 1: 0 (older records) = the default. */
    struct {
        uint16_t fade_on_ms;
        uint16_t fade_off_ms;
        uint16_t hold_ms;
        uint8_t  soft_end_pct;
        uint8_t  hold_end_pct;
    } pf[DIMMER_CHANNELS];
    uint8_t reserved[4];
} saved_t;

_Static_assert(sizeof(saved_t) == STORE_DATA_SIZE, "saved_t must match the store record");

static int8_t   g_last_hold_dir[DIMMER_CHANNELS];
static uint8_t  g_holding[DIMMER_CHANNELS];
static saved_t  g_saved;          /* what flash holds now */
static uint8_t  g_dirty;          /* something changed that may need saving */
static uint8_t  g_dirty_restart;  /* a new change: restart the delay */
static uint32_t g_dirty_ms;
static uint8_t  g_save_retries;

static uint16_t clamp_level(uint32_t v);

/* A level within 10..1000 and the channel's min..max. */
static uint16_t in_range(const dimmer_channel_t *c, uint32_t v) {
    uint16_t l = clamp_level(v);
    if (l < c->min) {
        l = c->min;
    }
    if (l > c->max) {
        l = c->max;
    }
    return l;
}

static void snapshot(saved_t *s) {
    memset(s, 0, sizeof(*s));
    s->format     = SAVED_FORMAT;
    s->power_on   = g_power_on[0];
    s->power_on_2 = g_power_on[1];
    for (uint8_t i = 0; i < DIMMER_CHANNELS; i++) {
        s->ch[i].level       = g_ch[i].level;
        s->ch[i].min         = g_ch[i].min;
        s->ch[i].max         = g_ch[i].max;
        s->ch[i].switch_type = g_ch[i].switch_type;
        s->ch[i].curve       = g_ch[i].curve;
        s->pf[i].fade_on_ms   = (uint16_t)(g_ch[i].fade_on_ms + 1u);
        s->pf[i].fade_off_ms  = (uint16_t)(g_ch[i].fade_off_ms + 1u);
        s->pf[i].hold_ms      = (uint16_t)(g_ch[i].hold_ms + 1u);
        s->pf[i].soft_end_pct = (uint8_t)(g_ch[i].soft_end_pct + 1u);
        s->pf[i].hold_end_pct = (uint8_t)(g_ch[i].hold_end_pct + 1u);
        if (g_power_on[i] == POWER_ON_LAST && g_ch[i].on) {
            s->on_mask |= (uint8_t)(1u << i);
        }
    }
}

/* The light state in the mailbox, kept current: after a reset that keeps the
 * RAM (update, watchdog, fault) dimmer_init brings it back. Bit ch: on; 10 bits
 * of level per channel from bit 2 (levels are 10..1000, on 0 or 1). */
static void mailbox_keep(void) {
    _Static_assert(DIMMER_CHANNELS == 2 && DIMMER_LEVEL_MAX < 1024, "restore word layout");
    uint32_t r = g_ch[0].on | (uint32_t)g_ch[1].on << 1 | (uint32_t)g_ch[0].level << 2 |
                 (uint32_t)g_ch[1].level << 12;
    OTA_MAILBOX->restore       = r;
    OTA_MAILBOX->restore_check = r ^ OTA_RESTORE_MAGIC;
}

static void apply(uint8_t ch) {
    dim_profile_t p;
    dim_profile_set(&p, g_ch[ch].fade_on_ms, g_ch[ch].fade_off_ms, g_ch[ch].soft_end_pct,
                    g_ch[ch].hold_ms, g_ch[ch].hold_end_pct);
    phase_set_profile(ch, &p);
    phase_set_range(ch, g_ch[ch].min, g_ch[ch].max);
    phase_set_curve(ch, g_ch[ch].curve);
    phase_set(ch, g_ch[ch].on, g_ch[ch].level);
    mailbox_keep();
}

/* A hold in progress ends without taking its level (the network or a setting
 * changed the channel under the finger). */
static void end_hold(uint8_t ch) {
    if (g_holding[ch]) {
        g_holding[ch] = 0;
        (void)phase_hold_stop(ch);
    }
}

static void mark_dirty(void) {
    g_dirty         = 1;
    g_dirty_restart = 1;
}

void dimmer_init(uint32_t reset_csr) {
    for (uint8_t i = 0; i < DIMMER_CHANNELS; i++) {
        /* Factory defaults of the stock firmware. */
        g_ch[i].on          = 0;
        g_ch[i].level       = DIMMER_LEVEL_MAX;
        g_ch[i].min         = 200;
        g_ch[i].max         = DIMMER_LEVEL_MAX;
        g_ch[i].switch_type = 0;
        g_ch[i].curve       = CURVE_LED_SOFT;   /* own default; saved records keep theirs */
        g_ch[i].countdown   = 0;
        g_ch[i].fade_on_ms   = DIM_FADE_ON_MS;
        g_ch[i].fade_off_ms  = DIM_FADE_OFF_MS;
        g_ch[i].soft_end_pct = DIM_SOFT_END_PCT;
        g_ch[i].hold_ms      = DIM_HOLD_MS;
        g_ch[i].hold_end_pct = DIM_HOLD_END_PCT;
    }
    g_power_on[0] = POWER_ON_OFF;
    g_power_on[1] = POWER_ON_OFF;

    saved_t s;
    if (store_load((uint8_t *)&s) && s.format >= 1 && s.format <= SAVED_FORMAT) {
        g_power_on[0] = s.power_on <= POWER_ON_LAST ? s.power_on : POWER_ON_OFF;
        g_power_on[1] = s.format == 1 ? g_power_on[0]
                      : s.power_on_2 <= POWER_ON_LAST ? s.power_on_2 : POWER_ON_OFF;
        for (uint8_t i = 0; i < DIMMER_CHANNELS; i++) {
            g_ch[i].min         = clamp_level(s.ch[i].min);
            g_ch[i].max         = clamp_level(s.ch[i].max);
            g_ch[i].switch_type = s.ch[i].switch_type <= 2 ? s.ch[i].switch_type : 0;
            g_ch[i].curve       = s.ch[i].curve < CURVE_COUNT ? s.ch[i].curve : 0;
            if (s.format >= 3) {              /* stored + 1; 0 = default */
                if (s.pf[i].fade_on_ms && s.pf[i].fade_on_ms <= DIM_PROFILE_MS_MAX + 1u) {
                    g_ch[i].fade_on_ms = (uint16_t)(s.pf[i].fade_on_ms - 1u);
                }
                if (s.pf[i].fade_off_ms && s.pf[i].fade_off_ms <= DIM_PROFILE_MS_MAX + 1u) {
                    g_ch[i].fade_off_ms = (uint16_t)(s.pf[i].fade_off_ms - 1u);
                }
                if (s.pf[i].hold_ms && s.pf[i].hold_ms <= DIM_PROFILE_MS_MAX + 1u) {
                    g_ch[i].hold_ms = (uint16_t)(s.pf[i].hold_ms - 1u);
                }
                if (s.pf[i].soft_end_pct && s.pf[i].soft_end_pct <= 101u) {
                    g_ch[i].soft_end_pct = (uint8_t)(s.pf[i].soft_end_pct - 1u);
                }
                if (s.pf[i].hold_end_pct && s.pf[i].hold_end_pct <= 101u) {
                    g_ch[i].hold_end_pct = (uint8_t)(s.pf[i].hold_end_pct - 1u);
                }
            }
            if (g_ch[i].min > g_ch[i].max) {
                g_ch[i].min = g_ch[i].max;
            }
            g_ch[i].level = in_range(&g_ch[i], s.ch[i].level);
            g_ch[i].on    = g_power_on[i] == POWER_ON_ON ||
                            (g_power_on[i] == POWER_ON_LAST && (s.on_mask & (1u << i)));
        }
    }
    snapshot(&g_saved);               /* what flash holds (a restore below is not in it) */

    /* Back from a reset that kept the RAM and was not a power-up or brown-out:
     * a firmware update (or the bootloader giving up on one), the watchdog or a
     * fault. The light comes back as it was, not by the power-on behaviour; but
     * once in a row only: a reset again before RESTORE_CLEAR_MS takes the
     * power-on behaviour, so a state that crashes is not brought back for ever. */
    volatile ota_mailbox_t *mb = OTA_MAILBOX;
    if (!(reset_csr & (DIMMER_RESET_SOFT | DIMMER_RESET_WDG)) || (reset_csr & DIMMER_RESET_POWER)) {
        mb->restored = 0;
    } else if (mb->restored != OTA_RESTORED_MAGIC &&
               (mb->restore ^ OTA_RESTORE_MAGIC) == mb->restore_check) {
        uint32_t r = mb->restore;
        for (uint8_t i = 0; i < DIMMER_CHANNELS; i++) {
            g_ch[i].on    = (uint8_t)((r >> i) & 1u);
            g_ch[i].level = in_range(&g_ch[i], (r >> (2 + 10 * i)) & 0x3FFu);
        }
        mb->restored = OTA_RESTORED_MAGIC;
    }
    for (uint8_t i = 0; i < DIMMER_CHANNELS; i++) {
        apply(i);                     /* also puts the state in the mailbox */
    }
}

void dimmer_prepare_restart(void) {
    for (uint8_t i = 0; i < DIMMER_CHANNELS; i++) {
        if (g_holding[i]) {           /* a hold in progress: where the light is now */
            g_holding[i]  = 0;
            g_ch[i].level = in_range(&g_ch[i], phase_current(i));
        }
    }
    saved_t s;
    snapshot(&s);
    if (memcmp(&s, &g_saved, sizeof(s)) != 0 && store_save((const uint8_t *)&s)) {
        g_saved = s;
    }
    mailbox_keep();
    OTA_MAILBOX->restored = 0;        /* a planned restart: restored after it in any case */
}

void dimmer_poll(uint32_t now, uint32_t rx_quiet_ms, uint8_t stall_ok) {
    if (now >= RESTORE_CLEAR_MS) {
        OTA_MAILBOX->restored = 0;    /* this run lasted: a reset may restore again */
    }
    if (g_dirty_restart) {
        g_dirty_restart = 0;
        g_dirty_ms      = now;
    }
    if (!g_dirty || now - g_dirty_ms < SAVE_DELAY_MS || rx_quiet_ms < SAVE_QUIET_MS || !stall_ok) {
        return;
    }
    g_dirty = 0;

    saved_t s;
    snapshot(&s);
    if (memcmp(&s, &g_saved, sizeof(s)) == 0) {
        return;
    }
    if (store_save((const uint8_t *)&s)) {
        g_saved        = s;
        g_save_retries = 0;
    } else if (++g_save_retries < SAVE_RETRIES) {
        g_dirty = 1;                 /* again at the next window, on the next page */
    } else {
        g_save_retries = 0;          /* give up until the next change */
    }
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
    *ch  = (uint8_t)((unsigned)(dpid - 1) / DP_PER_CHANNEL);   /* unsigned: no signed division code */
    *idx = (uint8_t)((unsigned)(dpid - 1) % DP_PER_CHANNEL);
    return 1;
}

uint8_t dimmer_write_dp(uint8_t dpid, uint8_t type, const uint8_t *value, uint16_t len) {
    uint8_t ch, idx;

    if (dpid == DP_POWER_ON ||
        (dpid >= DP_POWER_ON_BASE && dpid < DP_POWER_ON_BASE + DIMMER_CHANNELS)) {
        if (type != TUYA_DP_TYPE_ENUM || len != 1 || value[0] > POWER_ON_LAST) {
            return 0;
        }
        for (uint8_t i = 0; i < DIMMER_CHANNELS; i++) {
            if (dpid == DP_POWER_ON || dpid == DP_POWER_ON_BASE + i) {
                g_power_on[i] = value[0];     /* DP 14 (stock): both channels */
            }
        }
        mark_dirty();
        return 1;
    }
    if (dpid >= DP_PROFILE_BASE && dpid < DP_PROFILE_BASE + DP_PROFILE_COUNT * DIMMER_CHANNELS) {
        if (type != TUYA_DP_TYPE_VALUE || len != 4) {
            return 0;
        }
        uint32_t          v  = be32(value);
        uint8_t           k  = (uint8_t)((unsigned)(dpid - DP_PROFILE_BASE) % DP_PROFILE_COUNT);
        dimmer_channel_t *pc = &g_ch[(unsigned)(dpid - DP_PROFILE_BASE) / DP_PROFILE_COUNT];
        if (k == PF_SOFT_END || k == PF_HOLD_END) {
            if (v > 100u) {
                return 0;
            }
            if (k == PF_SOFT_END) {
                pc->soft_end_pct = (uint8_t)v;
            } else {
                pc->hold_end_pct = (uint8_t)v;
            }
        } else {
            if (v > DIM_PROFILE_MS_MAX) {
                return 0;
            }
            if (k == PF_FADE_ON) {
                pc->fade_on_ms = (uint16_t)v;
            } else if (k == PF_FADE_OFF) {
                pc->fade_off_ms = (uint16_t)v;
            } else {
                pc->hold_ms = (uint16_t)v;
            }
        }
        apply((uint8_t)((unsigned)(dpid - DP_PROFILE_BASE) / DP_PROFILE_COUNT));
        mark_dirty();
        return 1;
    }
    if (dpid >= DP_CURVE_BASE && dpid < DP_CURVE_BASE + DIMMER_CHANNELS) {
        if (type != TUYA_DP_TYPE_ENUM || len != 1 || value[0] >= CURVE_COUNT) {
            return 0;
        }
        ch               = (uint8_t)(dpid - DP_CURVE_BASE);
        g_ch[ch].curve   = value[0];
        apply(ch);
        mark_dirty();
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
        /* An off ends a wall switch hold, an on only a hold left from before an
         * off: the module writes DP 1 = 1 before every level change, so an on
         * to a lit channel must not take the light from under the finger. */
        if (!value[0] || !c->on) {
            end_hold(ch);
        }
        c->on = value[0] ? 1 : 0;
        apply(ch);
        mark_dirty();   /* switching off fixes the level to keep */
        return 1;

    case DP_SWITCH_TYPE:
        if (type != TUYA_DP_TYPE_ENUM || len != 1 || value[0] > 2) {
            return 0;
        }
        c->switch_type = value[0];
        end_hold(ch);
        apply(ch);
        mark_dirty();
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
         * the next "on", and it stays within min..max (the ack reports the
         * level taken, so the module learns it). */
        c->level = in_range(c, v);
        apply(ch);
        if (!c->on) {
            mark_dirty();
        }
        return 1;

    /* The other limit and the level follow (reported with it, dimmer_sibling_dp). */
    case DP_MIN:
        c->min = clamp_level(v);
        if (c->min > c->max) {
            c->max = c->min;
        }
        break;

    case DP_MAX:
        c->max = clamp_level(v);
        if (c->max < c->min) {
            c->min = c->max;
        }
        break;

    case DP_COUNTDOWN:
        c->countdown = v;
        return 1;

    default:
        return 0;
    }
    c->level = in_range(c, c->level);
    apply(ch);
    mark_dirty();
    return 1;
}

uint8_t dimmer_sibling_dp(uint8_t dpid, uint8_t n) {
    uint8_t ch, idx;
    if (dpid == DP_POWER_ON) {                /* DP 14 set both channels */
        return n < DIMMER_CHANNELS ? (uint8_t)(DP_POWER_ON_BASE + n) : 0;
    }
    if (n > 1 || !channel_dp(dpid, &ch, &idx) || (idx != DP_MIN && idx != DP_MAX)) {
        return 0;
    }
    if (n == 1) {
        return (uint8_t)(dpid - idx + DP_LEVEL);    /* the level, kept within min..max */
    }
    return (uint8_t)(idx == DP_MIN ? dpid + (DP_MAX - DP_MIN) : dpid - (DP_MAX - DP_MIN));
}

void dimmer_report_dp(uint8_t dpid) {
    uint8_t ch, idx;

    if (dpid == DP_POWER_ON) {
        tuya_report_enum(dpid, g_power_on[0]);
        return;
    }
    if (dpid >= DP_PROFILE_BASE && dpid < DP_PROFILE_BASE + DP_PROFILE_COUNT * DIMMER_CHANNELS) {
        const dimmer_channel_t *pc = &g_ch[(unsigned)(dpid - DP_PROFILE_BASE) / DP_PROFILE_COUNT];
        uint32_t                v;
        switch ((unsigned)(dpid - DP_PROFILE_BASE) % DP_PROFILE_COUNT) {
        case PF_FADE_ON:  v = pc->fade_on_ms;   break;
        case PF_FADE_OFF: v = pc->fade_off_ms;  break;
        case PF_SOFT_END: v = pc->soft_end_pct; break;
        case PF_HOLD:     v = pc->hold_ms;      break;
        default:          v = pc->hold_end_pct; break;
        }
        tuya_report_value(dpid, v);
        return;
    }
    if (dpid >= DP_POWER_ON_BASE && dpid < DP_POWER_ON_BASE + DIMMER_CHANNELS) {
        tuya_report_enum(dpid, g_power_on[dpid - DP_POWER_ON_BASE]);
        return;
    }
    if (dpid >= DP_CURVE_BASE && dpid < DP_CURVE_BASE + DIMMER_CHANNELS) {
        tuya_report_enum(dpid, g_ch[dpid - DP_CURVE_BASE].curve);
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

static void report_local(uint8_t ch, uint8_t with_level) {
    uint8_t base = (uint8_t)(ch * DP_PER_CHANNEL + 1);

    tuya_report_begin(0);
    dimmer_report_dp((uint8_t)(base + DP_ON));
    if (with_level) {
        dimmer_report_dp((uint8_t)(base + DP_LEVEL));
    }
    tuya_report_send();
}

void dimmer_local_set(uint8_t ch, uint8_t on) {
    if (ch >= DIMMER_CHANNELS || g_ch[ch].on == (on ? 1 : 0)) {
        return;
    }
    if (!on) {
        end_hold(ch);                 /* e.g. the module button during a hold */
    }
    g_ch[ch].on = on ? 1 : 0;
    apply(ch);
    mark_dirty();
    report_local(ch, 0);
}

void dimmer_local_toggle_all(void) {
    uint8_t any_on = 0;
    for (uint8_t ch = 0; ch < DIMMER_CHANNELS; ch++) {
        any_on |= g_ch[ch].on;
    }
    for (uint8_t ch = 0; ch < DIMMER_CHANNELS; ch++) {
        dimmer_local_set(ch, !any_on);        /* any on: all off, else all on */
    }
}

void dimmer_report_action(uint8_t ch, uint8_t gesture) {
    if (ch < DIMMER_CHANNELS && gesture) {
        tuya_report_begin(0);
        tuya_report_enum((uint8_t)(DP_ACTION_BASE + ch), gesture);
        tuya_report_send();
    }
}

void dimmer_boot_set(uint8_t ch, uint8_t on) {
    if (ch < DIMMER_CHANNELS) {
        g_ch[ch].on = on ? 1 : 0;
        apply(ch);
    }
}

void dimmer_local_toggle(uint8_t ch) {
    if (ch < DIMMER_CHANNELS) {
        dimmer_local_set(ch, !g_ch[ch].on);
    }
}

void dimmer_hold_begin(uint8_t ch, int8_t dir, uint8_t from_dark) {
    if (ch >= DIMMER_CHANNELS || !g_ch[ch].on) {
        return;
    }
    dimmer_channel_t *c   = &g_ch[ch];
    uint16_t          now = phase_current(ch);

    if (dir == 0) {
        if (from_dark || now <= c->min + 2 * DIM_HOLD_RATE) {
            dir = +1;
        } else if (now + 2 * DIM_HOLD_RATE >= c->max) {
            dir = -1;
        } else if (g_last_hold_dir[ch]) {
            dir = (int8_t)-g_last_hold_dir[ch];
        } else {                      /* the first hold: towards the far end */
            dir = now > (unsigned)(c->min + c->max) / 2u ? -1 : +1;
        }
    }
    g_last_hold_dir[ch] = dir;
    g_holding[ch]       = 1;
    phase_hold_start(ch, dir, c->min, c->max, from_dark);
}

void dimmer_hold_end(uint8_t ch) {
    if (ch >= DIMMER_CHANNELS || !g_holding[ch]) {
        return;
    }
    g_holding[ch] = 0;
    uint16_t v    = phase_hold_stop(ch);
    if (!g_ch[ch].on) {
        apply(ch);                  /* switched off during the hold: stay off */
        return;
    }
    g_ch[ch].level = in_range(&g_ch[ch], v);
    apply(ch);
    mark_dirty();                   /* a level picked by hand is kept (the stock MCU does too) */
    report_local(ch, 1);
}

void dimmer_hold_cancel(uint8_t ch) {
    if (ch >= DIMMER_CHANNELS || !g_holding[ch]) {
        return;
    }
    g_holding[ch] = 0;
    phase_hold_cancel(ch);
    apply(ch);                      /* target: the level it has, or dark if switched off meanwhile */
}

void dimmer_report_all(void) {
    /* DP 14 first, each channel's power-on (103 / 104) after it, so a module
     * that sets both channels from DP 14 ends with each channel's own value.
     * A module that has not identified us yet (0xA3) takes DP 14 only: the
     * first-contact dump comes before the identity answer, so the fork's ZTU
     * reads the whole dump again once it knows us. */
    tuya_report_begin(1);
    dimmer_report_dp(DP_POWER_ON);
    tuya_report_send();
    for (uint8_t ch = 0; ch < DIMMER_CHANNELS; ch++) {
        tuya_report_begin(1);
        for (uint8_t i = 0; i < DP_PER_CHANNEL; i++) {
            dimmer_report_dp((uint8_t)(ch * DP_PER_CHANNEL + i + 1));
        }
        dimmer_report_dp((uint8_t)(DP_CURVE_BASE + ch));
        dimmer_report_dp((uint8_t)(DP_POWER_ON_BASE + ch));
        tuya_report_send();
        tuya_report_begin(1);
        for (uint8_t k = 0; k < DP_PROFILE_COUNT; k++) {
            dimmer_report_dp((uint8_t)(DP_PROFILE_BASE + DP_PROFILE_COUNT * ch + k));
        }
        tuya_report_send();
    }
}
