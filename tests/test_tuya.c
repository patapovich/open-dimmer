/* Host test of src/tuya.c: the frame parser (back to back, resync, the stale
 * timeout, the version byte), the update offer (0xA0 NOTIFY), the identity
 * and version answers and the network query. The dimmer is a stub that
 * records the writes and answers each with one DP. Built three times: normal,
 * MEASURE=1 and DIAG_REPORT=1 (those decline every offer). Run: make test */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "dimmer.h"
#include "ota_proto.h"
#include "phase.h"
#include "tuya.h"
#include "uart.h"

#ifndef MEASURE
#define MEASURE 0
#endif
#ifndef DIAG_REPORT
#define DIAG_REPORT 0
#endif
#define DECLINES_ALL (MEASURE || DIAG_REPORT)

static int g_failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            g_failures++;                                                \
        }                                                                \
    } while (0)

/* ---- stubs: UART, dimmer, phase, reset -------------------------------------- */
volatile ota_mailbox_t g_host_mailbox;
static uint8_t  g_rxq[4096];
static unsigned g_rxh, g_rxt;
static uint8_t  g_tx[70000 * 9];
static unsigned g_txn;
static int      g_resets, g_shutdowns, g_prepares, g_order_ok;
static int      g_writes, g_dumps;
static uint8_t  g_last_dp;

int uart_getc(void) {
    return g_rxt == g_rxh ? -1 : g_rxq[g_rxt++];
}
void uart_putc(uint8_t b) {
    if (g_txn < sizeof(g_tx)) {
        g_tx[g_txn++] = b;
    }
}
void uart_flush(void) {
}
uint32_t uart_dropped(void) {
    return 0;
}

uint8_t dimmer_write_dp(uint8_t dpid, uint8_t type, const uint8_t *v, uint16_t len) {
    (void)type;
    (void)v;
    (void)len;
    g_writes++;
    g_last_dp = dpid;
    return 1;
}
void dimmer_report_dp(uint8_t dpid) {
    tuya_report_bool(dpid, 1);
}
uint8_t dimmer_sibling_dp(uint8_t dpid, uint8_t n) {
    (void)dpid;
    (void)n;
    return 0;
}
void dimmer_report_all(void) {
    g_dumps++;
}
void dimmer_prepare_restart(void) {
    g_prepares++;
    g_order_ok = g_shutdowns == 1;          /* gates off first */
}
void phase_shutdown(void) {
    g_shutdowns++;
}
void NVIC_SystemReset(void) {
    g_resets++;
}

/* ---- frames ----------------------------------------------------------------- */
static unsigned frame(uint8_t *o, uint8_t ver, uint8_t cmd, uint16_t seq, const uint8_t *d, uint16_t n) {
    unsigned i = 0;
    o[i++] = 0x55;
    o[i++] = 0xAA;
    o[i++] = ver;
    o[i++] = (uint8_t)(seq >> 8);
    o[i++] = (uint8_t)seq;
    o[i++] = cmd;
    o[i++] = (uint8_t)(n >> 8);
    o[i++] = (uint8_t)n;
    memcpy(o + i, d, n);
    i += n;
    uint8_t s = 0;
    for (unsigned k = 0; k < i; k++) {
        s = (uint8_t)(s + o[k]);
    }
    o[i++] = s;
    return i;
}

static void feed(const uint8_t *p, unsigned n) {
    memcpy(g_rxq + g_rxh, p, n);
    g_rxh += n;
}

static void feed_frame(uint8_t cmd, uint16_t seq, const uint8_t *d, uint16_t n) {
    uint8_t f[128];
    feed(f, frame(f, 0x02, cmd, seq, d, n));
}

typedef struct {
    uint8_t  ver, cmd;
    uint16_t seq, len;
    const uint8_t *data;
} sent_t;

/* The k-th frame the MCU sent since the last clear; 0 if there is none. */
static int sent(unsigned k, sent_t *f) {
    unsigned i = 0;
    while (i + 9 <= g_txn) {
        uint16_t n = (uint16_t)((g_tx[i + 6] << 8) | g_tx[i + 7]);
        uint8_t  s = 0;
        for (unsigned j = 0; j < 8u + n; j++) {
            s = (uint8_t)(s + g_tx[i + j]);
        }
        if (g_tx[i] != 0x55 || g_tx[i + 1] != 0xAA || s != g_tx[i + 8 + n]) {
            printf("FAIL: the MCU sent a broken frame at %u\n", i);
            g_failures++;
            return 0;
        }
        if (k-- == 0) {
            f->ver  = g_tx[i + 2];
            f->seq  = (uint16_t)((g_tx[i + 3] << 8) | g_tx[i + 4]);
            f->cmd  = g_tx[i + 5];
            f->len  = n;
            f->data = &g_tx[i + 8];
            return 1;
        }
        i += 9u + n;
    }
    return 0;
}

static unsigned sent_count(void) {
    sent_t   f;
    unsigned n = 0;
    while (sent(n, &f)) {
        n++;
    }
    return n;
}

static uint32_t g_now = 100000;

static void fresh(void) {
    tuya_init();
    g_rxh = g_rxt = 0;
    g_txn         = 0;
    g_writes = g_dumps = g_resets = g_shutdowns = g_prepares = g_order_ok = 0;
    g_now += 1000;
    tuya_poll(g_now);
    memset((void *)&g_host_mailbox, 0, sizeof(g_host_mailbox));
}

static const uint8_t k_dp1_on[5] = { 1, 1, 0, 1, 1 };

/* ---- parser ----------------------------------------------------------------- */
static void test_back_to_back_frames(void) {
    fresh();
    feed_frame(TUYA_CMD_WRITE, 1, k_dp1_on, 5);
    feed_frame(TUYA_CMD_WRITE, 2, k_dp1_on, 5);
    feed_frame(TUYA_CMD_QUERY_ALL, 3, NULL, 0);
    tuya_poll(g_now);
    CHECK(g_writes == 2 && g_dumps == 1);
    sent_t f;
    CHECK(sent_count() == 2);
    CHECK(sent(0, &f) && f.cmd == TUYA_CMD_REPORT_PASSIVE && f.ver == 0x02 && f.len == 5);
}

/* A header with a damaged length that never completes: after the stale
 * timeout only its sync bytes go, and the good frame behind it is taken. */
static void test_stale_header_keeps_the_frame_behind_it(void) {
    fresh();
    const uint8_t bad[8] = { 0x55, 0xAA, 0x02, 0, 7, TUYA_CMD_WRITE, 0x00, 0x50 };
    feed(bad, 8);
    feed_frame(TUYA_CMD_WRITE, 2, k_dp1_on, 5);
    tuya_poll(g_now);
    CHECK(g_writes == 0);                        /* still waiting for 0x50 bytes */
    CHECK(tuya_rx_quiet_ms(g_now + 10) == 0);    /* half a frame: no save */
    tuya_poll(g_now + 30);
    CHECK(g_writes == 0);
    tuya_poll(g_now + 60);                       /* stale */
    CHECK(g_writes == 1);
    tuya_poll(g_now + 61);
    CHECK(tuya_rx_quiet_ms(g_now + 70) != 0);    /* nothing half received now */
    /* Traffic goes on normally. */
    feed_frame(TUYA_CMD_QUERY_ALL, 4, NULL, 0);
    tuya_poll(g_now + 100);
    CHECK(g_dumps == 1);
}

/* Two damaged headers in a row: each stale step drops two bytes and looks
 * again, until the good frame is found. */
static void test_stale_headers_in_a_row(void) {
    fresh();
    const uint8_t bad[16] = { 0x55, 0xAA, 0x02, 0, 7, TUYA_CMD_WRITE, 0x00, 0x40,
                              0x55, 0xAA, 0x02, 0, 8, TUYA_CMD_WRITE, 0x00, 0x30 };
    feed(bad, 16);
    feed_frame(TUYA_CMD_WRITE, 2, k_dp1_on, 5);
    tuya_poll(g_now);
    tuya_poll(g_now + 51);                       /* the first header goes */
    CHECK(g_writes == 0);
    tuya_poll(g_now + 52);                       /* the second one */
    CHECK(g_writes == 1);
}

/* Only frame version 0x02 (what the module sends) is a frame. */
static void test_frame_version_byte(void) {
    static const uint8_t vers[3] = { 0x00, 0x03, 0x07 };
    for (int i = 0; i < 3; i++) {
        fresh();
        uint8_t f[64];
        feed(f, frame(f, vers[i], TUYA_CMD_WRITE, 1, k_dp1_on, 5));
        tuya_poll(g_now);
        CHECK(g_writes == 0);
        CHECK(sent_count() == 0);
        /* A real frame right behind it is taken. */
        feed_frame(TUYA_CMD_WRITE, 2, k_dp1_on, 5);
        tuya_poll(g_now);
        CHECK(g_writes == 1);
    }
}

static void test_damaged_frames_resync(void) {
    fresh();
    uint8_t f[64];
    unsigned n = frame(f, 0x02, TUYA_CMD_WRITE, 1, k_dp1_on, 5);
    f[n - 1] ^= 0x01;                            /* bad checksum */
    feed(f, n);
    const uint8_t big[8] = { 0x55, 0xAA, 0x02, 0, 1, TUYA_CMD_WRITE, 0x01, 0x00 };   /* over 96 */
    feed(big, 8);
    feed_frame(TUYA_CMD_WRITE, 2, k_dp1_on, 5);
    tuya_poll(g_now);
    CHECK(g_writes == 1);
}

/* ---- update offer ----------------------------------------------------------- */
static void offer(uint8_t version, uint32_t size, uint32_t crc, uint16_t len) {
    uint8_t d[9] = { version, (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8),
                     (uint8_t)size, (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8),
                     (uint8_t)crc };
    fresh();
    feed_frame(OTA_CMD_NOTIFY, 9, d, len);
    tuya_poll(g_now);
}

static int answered(uint8_t status) {
    sent_t f;
    return sent_count() == 1 && sent(0, &f) && f.cmd == OTA_CMD_NOTIFY && f.seq == 9 && f.len == 1 &&
           f.data[0] == status;
}

static void check_declined(void) {
    CHECK(answered(1));
    CHECK(g_resets == 0 && g_shutdowns == 0 && g_prepares == 0);
    CHECK(g_host_mailbox.magic == 0);
}

static void test_offer_newer_accepted(void) {
    offer((uint8_t)(FW_VERSION + 1), 11000, 0x12345678u, 9);
#if DECLINES_ALL
    check_declined();                            /* back to a normal build: SWD only */
#else
    CHECK(answered(0));
    CHECK(g_shutdowns == 1 && g_prepares == 1 && g_order_ok && g_resets == 1);
    CHECK(g_host_mailbox.magic == OTA_MAILBOX_MAGIC && g_host_mailbox.magic_inv == ~OTA_MAILBOX_MAGIC);
    CHECK(g_host_mailbox.expect_crc == 0x12345678u);
#endif
}

static void test_offers_declined(void) {
    offer(FW_VERSION, 11000, 1, 9);              /* not newer */
    check_declined();
    offer((uint8_t)(FW_VERSION - 1), 11000, 1, 9);
    check_declined();
    offer(0xBF, 11000, 1, 9);                    /* the SIM bench build */
    check_declined();
    offer(0xFF, 11000, 1, 9);                    /* never made */
    check_declined();
    offer((uint8_t)(FW_VERSION + 1), APP_HEADER_OFFSET - 1, 1, 9);
    check_declined();
    offer((uint8_t)(FW_VERSION + 1), APP_MAX_SIZE + 1, 1, 9);
    check_declined();
    offer((uint8_t)(FW_VERSION + 1), 11000, 1, 8); /* too short */
    check_declined();
}

static void test_offer_size_bounds_accepted(void) {
#if !DECLINES_ALL
    offer((uint8_t)(FW_VERSION + 1), APP_MAX_SIZE, 1, 9);
    CHECK(answered(0) && g_resets == 1);
    offer(0xC0, APP_HEADER_OFFSET, 1, 9);        /* 3.0.0: past the SIM code */
    CHECK(answered(0) && g_resets == 1);
#endif
}

/* ---- identity, version, network query --------------------------------------- */
static void test_ident_and_version_answers(void) {
    fresh();
    feed_frame(OTA_CMD_IDENT, 5, NULL, 0);
    feed_frame(TUYA_CMD_VERSION, 6, NULL, 0);
    tuya_poll(g_now);
    sent_t f;
    CHECK(sent_count() == 2);
    CHECK(sent(0, &f) && f.cmd == OTA_CMD_IDENT && f.seq == 5 && f.len == 7 &&
          memcmp(f.data, "ZDMS", 4) == 0 && f.data[4] == OTA_IDENT_KIND_APP &&
          f.data[5] == FW_VERSION && f.data[6] == OTA_PROTOCOL);
    CHECK(sent(1, &f) && f.cmd == TUYA_CMD_VERSION && f.seq == 6 && f.len == 1 && f.data[0] == FW_VERSION);
    /* No 0x0B of our own: only answers. */
    CHECK(g_txn == 16u + 10u);
}

/* Sequence 0 only for the very first query (the module's restart sign), also
 * after 65536 queries; the query also marks the line busy for the answer. */
static void test_network_query(void) {
    fresh();
    g_txn = 0;
    for (uint32_t i = 0; i < 65536u + 3u; i++) {
        tuya_query_network(g_now);
    }
    /* One pass over what was sent (sent() would count from the start each time). */
    unsigned zeros = 0, ones = 0, frames = 0;
    uint16_t prev = 0;
    int      steps_ok = 1;
    for (unsigned i = 0; i + 9 <= g_txn; i += 9, frames++) {
        uint16_t seq = (uint16_t)((g_tx[i + 3] << 8) | g_tx[i + 4]);
        uint8_t  s   = 0;
        for (unsigned j = 0; j < 8; j++) {
            s = (uint8_t)(s + g_tx[i + j]);
        }
        if (g_tx[i] != 0x55 || g_tx[i + 2] != 0x02 || g_tx[i + 5] != TUYA_CMD_QUERY_NETWORK ||
            g_tx[i + 6] != 0 || g_tx[i + 7] != 0 || g_tx[i + 8] != s) {
            steps_ok = 0;
        }
        if (seq == 0) {
            zeros++;
            steps_ok &= frames == 0;
        }
        ones += seq == 1;
        if (frames > 0 && seq != (uint16_t)(prev + 1u) && !(prev == 0xFFFF && seq == 1)) {
            steps_ok = 0;
        }
        prev = seq;
    }
    CHECK(g_txn == (65536u + 3u) * 9u);
    CHECK(zeros == 1 && ones == 2 && steps_ok);
    CHECK(prev == 3);

    /* A save (~5 ms stall) does not start while the answer is due. */
    fresh();
    g_now += 5000;
    tuya_poll(g_now);
    CHECK(tuya_rx_quiet_ms(g_now) >= 5000);
    tuya_query_network(g_now);
    CHECK(tuya_rx_quiet_ms(g_now + 20) == 20);
    /* The answer: the network state is taken, and the first one after our
     * start makes us tell our state (the dump). */
    const uint8_t joined = TUYA_NETWORK_CONNECTED;
    feed_frame(TUYA_CMD_QUERY_NETWORK, 0, &joined, 1);
    tuya_poll(g_now + 25);
    CHECK(tuya_network_status() == TUYA_NETWORK_CONNECTED);
    CHECK(tuya_rx_quiet_ms(g_now + 40) == 15);
}

int main(void) {
    test_back_to_back_frames();
    test_stale_header_keeps_the_frame_behind_it();
    test_stale_headers_in_a_row();
    test_frame_version_byte();
    test_damaged_frames_resync();
    test_offer_newer_accepted();
    test_offers_declined();
    test_offer_size_bounds_accepted();
    test_ident_and_version_answers();
    test_network_query();
    const char *build = MEASURE ? "MEASURE" : DIAG_REPORT ? "DIAG" : "normal";
    if (g_failures) {
        printf("test_tuya (%s build): %d FAILED\n", build, g_failures);
        return 1;
    }
    printf("test_tuya (%s build): all passed\n", build);
    return 0;
}
