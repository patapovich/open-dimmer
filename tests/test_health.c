/* Host test of src/health.c: report times, the uptime in minutes across the
 * wrap of the millisecond clock (49.7 days), the counters at full width, and
 * the bootloader's DPs 136 / 137 (bootif.c's values, here set by the test).
 * The report builder is a stub that records the DPs. Run: make test */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bootif.h"
#include "health.h"
#include "phase.h"
#include "store.h"
#include "tuya.h"
#include "uart.h"

static int g_failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            g_failures++;                                                \
        }                                                                \
    } while (0)

volatile phase_debug_t g_phase_dbg;
boot_info_t            g_boot_info;
static uint32_t        g_saves, g_uart_lost;
uint32_t store_save_count(void) {
    return g_saves;
}
uint32_t uart_dropped(void) {
    return g_uart_lost;
}

/* The last report: value per DP, and how many reports were sent. */
static uint32_t g_dp[256];
static uint8_t  g_dp_seen[256];
static unsigned g_reports, g_begun, g_passive, g_units;
void tuya_report_begin(uint8_t passive) {
    g_begun   = 1;
    g_passive = passive;
    g_units   = 0;
    memset(g_dp_seen, 0, sizeof(g_dp_seen));
}
void tuya_report_value(uint8_t dpid, uint32_t value) {
    CHECK(g_begun);
    g_dp[dpid]      = value;
    g_dp_seen[dpid] = 1;
    g_units++;
}
void tuya_report_send(void) {
    g_reports++;
    g_begun = 0;
}

int main(void) {
    health_init(0x0C);                    /* power + pin reset */
    g_phase_dbg.lock_losses = 300;        /* wider than a byte */
    g_phase_dbg.zc_ignored  = 70000;
    g_saves                 = 70000;      /* wider than 16 bits */
    g_uart_lost             = 5;
    g_boot_info.sector_crc  = 0xEAC45031u;    /* the released v1 bootloader */
    g_boot_info.word        = 0x009041u;      /* LINK, probe time 9 ms, tries 1 */

    /* First report a minute after start, then every 2 min. */
    uint32_t now = 0;
    for (; now < 60000u; now += 10) {
        health_poll(now);
    }
    CHECK(g_reports == 0);
    health_poll(now);
    CHECK(g_reports == 1 && g_passive == 1);
    CHECK(g_dp_seen[130] && g_dp[130] == 1);
    CHECK(g_dp_seen[131] && g_dp[131] == 0x0C);
    CHECK(g_dp_seen[132] && g_dp[132] == 300);
    CHECK(g_dp_seen[133] && g_dp[133] == 70000);
    CHECK(g_dp_seen[134] && g_dp[134] == 70000);
    CHECK(g_dp_seen[135] && g_dp[135] == 5);
    CHECK(g_dp_seen[136] && g_dp[136] == 0xEAC45031u);
    CHECK(g_dp_seen[137] && g_dp[137] == 0x009041u);
    for (unsigned dp = 0; dp < 256; dp++) {
        CHECK(g_dp_seen[dp] == (dp >= 130 && dp <= 137));
    }
    /* 8 value DPs of 8 bytes: one frame of the report builder's 64 bytes. */
    CHECK(g_units == 8 && g_units * 8u <= 64u);
    health_poll(now + 119999u);
    CHECK(g_reports == 1);
    health_poll(now + 120000u);
    CHECK(g_reports == 2 && g_dp[130] == 3);

    /* 60 days in 1 s steps: the uptime keeps counting minutes past the wrap
     * of the ms clock at 2^32 ms (49.7 days); a report every 2 min. */
    unsigned reports = g_reports, bad = 0;
    uint32_t last = g_dp[130];
    uint64_t t;
    for (t = (uint64_t)now + 120000u + 1000u; t <= 60ull * 86400000ull; t += 1000u) {
        health_poll((uint32_t)t);
        if (g_dp[130] != last) {
            uint32_t want = (uint32_t)(t / 60000u);
            if (g_dp[130] != want && g_dp[130] != want - 1u) {
                bad++;
            }
            last = g_dp[130];
        }
    }
    CHECK(bad == 0);
    CHECK(last >= 60u * 1440u - 2u && last <= 60u * 1440u);
    unsigned want_reports = (unsigned)((60ull * 86400000ull - 180000ull) / 120000ull);
    CHECK(g_reports - reports >= want_reports - 1 && g_reports - reports <= want_reports + 1);

    if (g_failures) {
        printf("test_health: %d FAILED\n", g_failures);
        return 1;
    }
    printf("test_health: all passed\n");
    return 0;
}
