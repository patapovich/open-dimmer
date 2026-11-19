#include "health.h"
#include "bootif.h"
#include "phase.h"
#include "py32f0xx.h"
#include "store.h"
#include "temp.h"
#include "tuya.h"
#include "uart.h"

#ifndef RING_HIGH
#define RING_HIGH 0                /* set by the Makefile with the linker layout */
#endif

#define HEALTH_FIRST_MS    60000u
#define HEALTH_EVERY_MS    120000u
#define MINUTE_MS          60000u

static uint32_t g_reset_cause;
static uint32_t g_health_next = HEALTH_FIRST_MS;
static uint32_t g_minute_next = MINUTE_MS;
static uint32_t g_uptime_min;

void health_init(uint32_t reset_cause) {
    g_reset_cause = reset_cause;
    temp_init();
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
    /* Bits 31..24 are the app's: 24 the settings ring is the upper half (RING_HIGH), 25 the
     * newest record is in it ("ring moved", the check before apps over 12 KB). */
    tuya_report_value(137, g_boot_info.word | (uint32_t)RING_HIGH << 24 |
                           (uint32_t)store_in_own_ring() << 25);
    tuya_report_send();
    tuya_report_begin(1);                             /* the report builder holds 8: a second frame */
    tuya_report_value(138, (uint32_t)(int32_t)temp_take());   /* degC, signed; -32768: none */
    tuya_report_send();
}
