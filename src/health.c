#include "health.h"
#include "bootif.h"
#include "phase.h"
#include "py32f0xx.h"
#include "store.h"
#include "tuya.h"
#include "uart.h"

#define HEALTH_FIRST_MS    60000u
#define HEALTH_EVERY_MS    120000u
#define MINUTE_MS          60000u

static uint32_t g_reset_cause;
static uint32_t g_health_next = HEALTH_FIRST_MS;
static uint32_t g_minute_next = MINUTE_MS;
static uint32_t g_uptime_min;

void health_init(uint32_t reset_cause) {
    g_reset_cause = reset_cause;
}

void health_poll(uint32_t now) {
    while ((int32_t)(now - g_minute_next) >= 0) {
        g_minute_next += MINUTE_MS;
        g_uptime_min++;
    }
    if ((int32_t)(now - g_health_next) < 0) {
        return;
    }
    g_health_next = now + HEALTH_EVERY_MS;
    __disable_irq();
    uint32_t losses  = g_phase_dbg.lock_losses;
    uint32_t ignored = g_phase_dbg.zc_ignored;
    __enable_irq();
    tuya_report_begin(1);
    tuya_report_value(130, g_uptime_min);
    tuya_report_value(131, g_reset_cause);
    tuya_report_value(132, losses);
    tuya_report_value(133, ignored);
    tuya_report_value(134, store_save_count());
    tuya_report_value(135, uart_dropped());
    tuya_report_value(136, g_boot_info.sector_crc);   /* 8 DPs: one 64-byte report */
    tuya_report_value(137, g_boot_info.word);
    tuya_report_send();
}
