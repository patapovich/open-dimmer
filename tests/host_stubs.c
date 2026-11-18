#include "host_stubs.h"
#include "dimmer.h"
#include "phase.h"
#include "tuya.h"
#include <string.h>

int g_fail;

volatile ota_mailbox_t g_host_mailbox;
GPIO_TypeDef           g_host_gpioa, g_host_gpiob;

/* ---- log ------------------------------------------------------------------ */

char          g_host_log[16384];
static size_t g_log_len;

static void log_add(const char *s) {
    size_t n = strlen(s);
    if (g_log_len + n + 1 < sizeof(g_host_log)) {
        memcpy(g_host_log + g_log_len, s, n + 1);
        g_log_len += n;
    } else {
        printf("FAIL host log full\n");
        g_fail = 1;
    }
}

void host_log_clear(void) {
    g_log_len     = 0;
    g_host_log[0] = 0;
}

long host_log_find(const char *what) {
    const char *p = strstr(g_host_log, what);
    return p ? (long)(p - g_host_log) : -1;
}

/* ---- tuya.c report builder and send ---------------------------------------- */

static char    g_frame[512];
static size_t  g_frame_len;
static uint8_t g_passive;

void tuya_report_begin(uint8_t passive) {
    g_passive   = passive;
    g_frame_len = 0;             /* as the real builder: anything unsent is dropped */
}

static void unit(uint8_t dpid, uint32_t v) {
    g_frame_len += (size_t)snprintf(g_frame + g_frame_len, sizeof(g_frame) - g_frame_len, "%s%u=%u",
                                    g_frame_len ? "," : "", dpid, (unsigned)v);
}

void tuya_report_bool(uint8_t dpid, uint8_t value) {
    unit(dpid, value ? 1u : 0u);
}

void tuya_report_enum(uint8_t dpid, uint8_t value) {
    unit(dpid, value);
}

void tuya_report_value(uint8_t dpid, uint32_t value) {
    unit(dpid, value);
}

void tuya_report_send(void) {
    if (g_frame_len) {
        char s[sizeof(g_frame) + 8];
        snprintf(s, sizeof(s), "%c[%s] ", g_passive ? 'P' : 'A', g_frame);
        log_add(s);
        g_frame_len = 0;
    }
}

void tuya_send(uint8_t cmd, uint16_t seq, const uint8_t *data, uint16_t len) {
    (void)seq;
    (void)data;
    (void)len;
    char s[8];
    snprintf(s, sizeof(s), "S%02X ", cmd);
    log_add(s);
}

void host_write(uint8_t dpid, uint8_t type, uint32_t value) {
    uint8_t  b[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
                      (uint8_t)value };
    uint8_t  one  = (uint8_t)value;
    tuya_report_begin(1);
    if (type == TUYA_DP_TYPE_VALUE) {
        (void)dimmer_write_dp(dpid, type, b, 4);
    } else {
        (void)dimmer_write_dp(dpid, type, &one, 1);
    }
    dimmer_report_dp(dpid);
    uint8_t sibling;
    for (uint8_t n = 0; (sibling = dimmer_sibling_dp(dpid, n)) != 0; n++) {
        dimmer_report_dp(sibling);
    }
    tuya_report_send();
}

/* ---- phase.c --------------------------------------------------------------- */

dim_fader_t g_host_fader[2];

void host_phase_init(void) {
    dim_fader_init(&g_host_fader[0]);
    dim_fader_init(&g_host_fader[1]);
}

void host_halfcycle(void) {
    (void)dim_fader_tick(&g_host_fader[0]);
    (void)dim_fader_tick(&g_host_fader[1]);
}

void phase_set(uint8_t ch, uint8_t on, uint16_t level) {
    dim_fader_set(&g_host_fader[ch], on, level);
}

void phase_set_range(uint8_t ch, uint16_t min, uint16_t max) {
    (void)max;
    dim_fader_set_floor(&g_host_fader[ch], min);
}

void phase_set_curve(uint8_t ch, uint8_t curve) {
    (void)ch;
    (void)curve;
}

void phase_hold_start(uint8_t ch, int8_t dir, uint16_t lo, uint16_t hi, uint8_t from_dark) {
    if (from_dark) {
        dim_fader_hold_start_from_dark(&g_host_fader[ch], lo, hi);
    } else {
        dim_fader_hold_start(&g_host_fader[ch], dir, lo, hi);
    }
}

uint16_t phase_hold_stop(uint8_t ch) {
    return dim_fader_hold_stop(&g_host_fader[ch]);
}

void phase_hold_cancel(uint8_t ch) {
    dim_fader_hold_cancel(&g_host_fader[ch]);
}

void phase_set_profile(uint8_t ch, const dim_profile_t *p) {
    dim_fader_set_profile(&g_host_fader[ch], p);
}

uint16_t phase_current(uint8_t ch) {
    return g_host_fader[ch].current;
}

/* ---- store.c --------------------------------------------------------------- */

uint8_t  g_host_flash[STORE_DATA_SIZE];
uint8_t  g_host_flash_valid;
unsigned g_host_saves;
uint8_t  g_host_save_attempts;

uint8_t store_load(uint8_t *data) {
    if (!g_host_flash_valid) {
        return 0;
    }
    memcpy(data, g_host_flash, STORE_DATA_SIZE);
    return 1;
}

uint8_t store_save_tries(const uint8_t *data, uint8_t attempts) {
    g_host_save_attempts = attempts;
    memcpy(g_host_flash, data, STORE_DATA_SIZE);
    g_host_flash_valid = 1;
    g_host_saves++;
    return 1;
}

uint32_t store_save_count(void) {
    return g_host_saves;
}
