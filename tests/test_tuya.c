/* Host test of src/tuya.c with src/bootif.c: the frame parser (back to back, resync, the stale
 * timeout, the version byte), the update offer (0xA0: the byte rule of a phase-1 module, the
 * build rule of a build-aware one, the app limit from the bootloader and the settings ring),
 * the identity (10 bytes, with the build and the bootloader version) and version answers, the
 * network query's sequence numbers, the stock module's product query 0x01 (never answered)
 * and the confirmation of a run in the boot word (docs/BOOTLOADER.md sections 2, 7, 9, 11).
 * The dimmer is a stub that records the writes and answers each with one DP. Built for every
 * build kind: normal, FW_TEST, FW_PREV, SIM, MEASURE and DIAG (the bench builds decline every
 * offer).
 * Each test runs in its own process: tuya.c keeps per-run state. Run: make test */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "bootif.h"
#include "controls.h"
#include "dimmer.h"
#include "host_boot.h"
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
#ifndef SIM_ZC
#define SIM_ZC 0
#endif
#define DECLINES_ALL (SIM_ZC || MEASURE || DIAG_REPORT)

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
uint8_t dimmer_dark(void) {
    return 1;
}
uint8_t controls_idle(void) {
    return 1;
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

/* What a v2 bootloader leaves after a normal boot of a counted app: tries 1. */
#define WORD_AT_START (BOOT_WORD_FRESH | (120u << BOOT_PROBE_MS_SHIFT) | 1u)

static void fresh(void) {
    tuya_init();
    g_rxh = g_rxt = 0;
    g_txn         = 0;
    g_writes = g_dumps = g_resets = g_shutdowns = g_prepares = g_order_ok = 0;
    g_now += 1000;
    tuya_poll(g_now);
    memset((void *)&g_host_mailbox, 0, sizeof(g_host_mailbox));
    host_boot_word(WORD_AT_START);
}

/* Each test from a fresh process (tuya.c and bootif.c keep per-run state), under a v2
 * bootloader with the 12 KB settings layout unless the test boots otherwise. */
static void boot_v2(uint32_t settings_start, uint32_t app_max) {
    host_boot_v2(BOOT_FEATURES_V2, app_max);
    g_host_settings_start = settings_start;
    host_boot_word(WORD_AT_START);
    bootif_init();
}

static void run(void (*test)(void), const char *name) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        g_failures = 0;
        boot_v2(SETTINGS_BASE_LEGACY, BOOT_APP_MAX);
        test();
        fflush(stdout);
        _exit(g_failures ? 1 : 0);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
        printf("FAIL %s\n", name);
        g_failures++;
    }
}
#define RUN(t) run(t, #t)

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
/* A phase-1 module's offer: 9 bytes (or len), byte rule. */
static void offer(uint8_t version, uint32_t size, uint32_t crc, uint16_t len) {
    uint8_t d[9] = { version, (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8),
                     (uint8_t)size, (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8),
                     (uint8_t)crc };
    fresh();
    feed_frame(OTA_CMD_NOTIFY, 9, d, len);
    tuya_poll(g_now);
}

/* A build-aware module's offer: 11 bytes, the carried build at 9-10 (len 10: build cut off). */
static void offer_build(uint8_t version, uint32_t size, uint32_t crc, uint16_t build, uint16_t len) {
    uint8_t d[11] = { version, (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8),
                      (uint8_t)size, (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8),
                      (uint8_t)crc, (uint8_t)(build >> 8), (uint8_t)build };
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
    CHECK(g_host_mailbox.magic == 0 && g_host_mailbox.magic_inv == 0 && g_host_mailbox.expect_crc == 0);
    CHECK(host_boot_word_now() == WORD_AT_START);   /* an offer never touches the boot word */
}

/* Accepted: gates off, the light state saved, then the update magic and the CRC to take, and
 * the restart; under a bench build declined like everything else. */
static void check_accepted(uint32_t crc) {
#if DECLINES_ALL
    (void)crc;
    check_declined();                            /* back to a normal build: SWD only */
#else
    CHECK(answered(0));
    CHECK(g_shutdowns == 1 && g_prepares == 1 && g_order_ok && g_resets == 1);
    CHECK(g_host_mailbox.magic == OTA_MAILBOX_MAGIC && g_host_mailbox.magic_inv == ~OTA_MAILBOX_MAGIC);
    CHECK(g_host_mailbox.expect_crc == crc);
    CHECK(host_boot_word_now() == WORD_AT_START);
#endif
}

static void test_offer_newer_byte_accepted(void) {
    offer((uint8_t)(FW_VERSION + 1), 11000, 0x12345678u, 9);
    check_accepted(0x12345678u);
}

static void test_offers_declined_byte_rule(void) {
    offer(FW_VERSION, 11000, 1, 9);              /* not newer */
    check_declined();
    offer((uint8_t)(FW_VERSION - 1), 11000, 1, 9);
    check_declined();
    offer(0xBF, 11000, 1, 9);                    /* the SIM bench build */
    check_declined();
    offer(0xFF, 11000, 1, 9);                    /* never made */
    check_declined();
    offer((uint8_t)(FW_VERSION + 1), APP_SIZE_MIN - 1, 1, 9);   /* below the header's end */
    check_declined();
    offer((uint8_t)(FW_VERSION + 1), APP_LIMIT_LEGACY + 1, 1, 9);
    check_declined();
    offer((uint8_t)(FW_VERSION + 1), 0xFFFFFFFFu, 1, 9);
    check_declined();
    offer((uint8_t)(FW_VERSION + 1), 11000, 1, 8); /* too short */
    check_declined();
    offer((uint8_t)(FW_VERSION + 1), 11000, 1, 0);
    check_declined();
}

/* The size bounds match the bootloader's (APP_SIZE_MIN = 0xD0, item 42) and the limit. */
static void test_offer_size_bounds(void) {
    offer((uint8_t)(FW_VERSION + 1), APP_LIMIT_LEGACY, 7, 9);
    check_accepted(7);
    offer((uint8_t)(FW_VERSION + 1), APP_SIZE_MIN, 8, 9);
    check_accepted(8);
    offer(0xC0, APP_SIZE_MIN, 9, 9);             /* 3.0.0: past the SIM code */
    check_accepted(9);
}

/* A build-aware module (11 bytes): the build decides, the byte does not. */
static void test_offer_build_rule(void) {
    offer_build(0x80, 11000, 0x2468ACE0u, (uint16_t)(FW_IMAGE_BUILD + 1u), 11);   /* older byte */
    if (FW_IMAGE_BUILD < OTA_BUILD_MAX) {
        check_accepted(0x2468ACE0u);
    } else {
        check_declined();                        /* SIM: 0xFFFE, nothing is newer */
    }
    offer_build(0x80, 11000, 3, OTA_BUILD_MAX, 11);
    if (FW_IMAGE_BUILD < OTA_BUILD_MAX) {
        check_accepted(3);
    } else {
        check_declined();
    }
    offer_build(0xFE, 11000, 1, (uint16_t)FW_IMAGE_BUILD, 11);   /* same build, newer byte */
    check_declined();
    offer_build(0xFE, 11000, 1, (uint16_t)(FW_IMAGE_BUILD - 1u), 11);
    check_declined();
    offer_build(0xFE, 11000, 1, OTA_BUILD_NONE, 11);             /* a 2.1.x image */
    check_declined();
    offer_build(0xFE, 11000, 1, 255, 11);                        /* never a build */
    check_declined();
    offer_build(0xFE, 11000, 1, OTA_BUILD_SIM, 11);
    check_declined();
    offer_build(0xFE, 11000, 1, OTA_BUILD_NEVER, 11);
    check_declined();
    offer_build(0x80, APP_SIZE_MIN - 1, 1, (uint16_t)(FW_IMAGE_BUILD + 1u), 11);
    check_declined();
    offer_build(0x80, APP_LIMIT_LEGACY + 1, 1, (uint16_t)(FW_IMAGE_BUILD + 1u), 11);
    check_declined();
    /* 10 bytes: no build yet, the byte rule. */
    offer_build((uint8_t)(FW_VERSION + 1), 11000, 4, OTA_BUILD_NONE, 10);
    check_accepted(4);
    offer_build(FW_VERSION, 11000, 4, (uint16_t)(FW_IMAGE_BUILD + 1u), 10);
    check_declined();
}

/* app_limit = min(_settings_start - APP_BASE, boot_id ? app_max : 12 KB). */
static void limit_case(uint32_t settings_start, int v2, uint32_t app_max, uint32_t want) {
    if (v2) {
        boot_v2(settings_start, app_max);
    } else {
        host_boot_v1();
        g_host_settings_start = settings_start;
        bootif_init();
    }
    CHECK(g_boot_info.app_limit == want);
    offer((uint8_t)(FW_VERSION + 1), want, 5, 9);
    check_accepted(5);
    offer((uint8_t)(FW_VERSION + 1), want + 1, 5, 9);
    check_declined();
    offer_build(0x80, want + 1, 5, (uint16_t)(FW_IMAGE_BUILD + 1u), 11);
    check_declined();
}

static void test_offer_app_limit(void) {
    limit_case(SETTINGS_BASE_LEGACY, 1, BOOT_APP_MAX, APP_LIMIT_LEGACY);   /* v2, today's ring */
    limit_case(SETTINGS_BASE_LEGACY, 0, 0, APP_LIMIT_LEGACY);              /* v1 */
    limit_case(SETTINGS_BASE_HIGH, 1, BOOT_APP_MAX, BOOT_APP_MAX);         /* release M on v2 */
    limit_case(SETTINGS_BASE_HIGH, 0, 0, APP_LIMIT_LEGACY);                /* ... on v1: 12 KB */
    limit_case(SETTINGS_BASE_LEGACY, 1, 0x2000u, 0x2000u);                 /* the smaller wins */
}

/* ---- identity, version ------------------------------------------------------ */
static void check_ident(uint8_t boot_version) {
    fresh();
    feed_frame(OTA_CMD_IDENT, 5, NULL, 0);
    feed_frame(TUYA_CMD_VERSION, 6, NULL, 0);
    tuya_poll(g_now);
    sent_t f;
    CHECK(sent_count() == 2);
    CHECK(sent(0, &f) && f.cmd == OTA_CMD_IDENT && f.seq == 5 && f.len == OTA_IDENT_LEN &&
          memcmp(f.data + OTA_IDENT_AT_MAGIC, "ZDMS", 4) == 0 &&
          f.data[OTA_IDENT_AT_KIND] == OTA_IDENT_KIND_APP && f.data[OTA_IDENT_AT_VERSION] == FW_VERSION &&
          f.data[OTA_IDENT_AT_PROTOCOL] == OTA_PROTOCOL &&
          f.data[OTA_IDENT_AT_BUILD] == (uint8_t)(FW_IMAGE_BUILD >> 8) &&
          f.data[OTA_IDENT_AT_BUILD + 1] == (uint8_t)FW_IMAGE_BUILD &&
          f.data[OTA_IDENT_AT_BOOT] == boot_version);
    CHECK(sent(1, &f) && f.cmd == TUYA_CMD_VERSION && f.seq == 6 && f.len == 1 && f.data[0] == FW_VERSION);
    /* No 0x0B of our own: only answers. */
    CHECK(g_txn == (8u + OTA_IDENT_LEN + 1u) + 10u);
}

static void test_ident_and_version_answers(void) {
    check_ident(BOOT_VERSION);                   /* v2: boot_id.version */
    host_boot_v1();
    bootif_init();
    check_ident(OTA_BOOT_VERSION_V1);            /* v1: no boot_id */
    CHECK(OTA_BOOT_VERSION_V1 == 1u && BOOT_VERSION == 2u);
}

/* The build this image carries (header, identity): the release build, + FW_TEST for a spare
 * test build (below the next release), - 100 + FW_TEST with FW_PREV (below this release),
 * 0xFFFE for SIM. (What must not compile: tests/test_build_rules.py.) */
static void test_build_number(void) {
#if SIM_ZC
    CHECK(FW_IMAGE_BUILD == OTA_BUILD_SIM && FW_VERSION == OTA_VERSION_SIM);
#else
    CHECK(FW_IMAGE_BUILD == FW_BUILD - OTA_BUILD_STEP * FW_PREV + FW_TEST);
    CHECK(FW_BUILD % OTA_BUILD_STEP == 0 && FW_TEST <= 99 && (FW_TEST >= 1 || !FW_PREV));
    CHECK(FW_IMAGE_BUILD >= OTA_BUILD_MIN && FW_IMAGE_BUILD <= OTA_BUILD_MAX);
    CHECK(FW_IMAGE_BUILD < FW_BUILD + OTA_BUILD_STEP);           /* below the next release */
    CHECK(!FW_PREV || FW_IMAGE_BUILD < FW_BUILD);                /* below this one */
    CHECK(FW_BUILD >= OTA_BUILD_FIRST);
#endif
}

/* ---- what the app reads of the bootloader (DPs 136, 137; identity byte 9) ------ */
static void test_boot_info(void) {
    /* v2: the boot_id, and the boot word at start (its low 24 bits) for DP 137. */
    host_boot_v2(BOOT_FEATURES_V2, BOOT_APP_MAX);
    host_boot_word(BOOT_WORD_FRESH | (1234u << BOOT_PROBE_MS_SHIFT) | BOOT_INFO_WRITTEN | 1u);
    bootif_init();
    CHECK(g_boot_info.version == BOOT_VERSION && g_boot_info.features == BOOT_FEATURES_V2);
    CHECK(g_boot_info.word == ((1234u << BOOT_PROBE_MS_SHIFT) | BOOT_INFO_WRITTEN | 1u));
    CHECK(g_boot_info.sector_crc == 0x3A7485B6u);              /* zlib.crc32 of that sector */
    CHECK(g_boot_info.sector_crc == host_boot_sector_crc());
    /* The word is read at start: a later confirmation does not change DP 137. */
    g_host_mailbox.boot = 0;
    CHECK(g_boot_info.word == ((1234u << BOOT_PROBE_MS_SHIFT) | BOOT_INFO_WRITTEN | 1u));
    /* No valid word (v1: random RAM, or torn): 0, not random bits that look like info. */
    host_boot_word(BOOT_WORD_FRESH | BOOT_INFO_RESCUE | 3u);
    g_host_mailbox.boot_check ^= 0x80000000u;
    bootif_init();
    CHECK(g_boot_info.word == 0);
    g_host_mailbox.boot       = 0x00FFFFFFu;                   /* tag 0 */
    g_host_mailbox.boot_check = 0xFF000000u;
    bootif_init();
    CHECK(g_boot_info.word == 0);
    /* v1: version 1, no features, its sector's CRC. */
    host_boot_v1();
    host_boot_word(BOOT_WORD_FRESH | 1u);
    bootif_init();
    CHECK(g_boot_info.version == OTA_BOOT_VERSION_V1 && g_boot_info.features == 0);
    CHECK(g_boot_info.sector_crc == 0x13A6F5B3u);              /* zlib.crc32 of that sector */
    /* A boot_id with a wrong magic is no boot_id. */
    host_boot_v2(BOOT_FEATURES_V2, BOOT_APP_MAX);
    ((uint8_t *)g_host_boot_sector)[BOOT_ID_ADDR - BOOT_BASE] ^= 1u;
    bootif_init();
    CHECK(g_boot_info.version == OTA_BOOT_VERSION_V1 && g_boot_info.features == 0 &&
          g_boot_info.app_limit == APP_LIMIT_LEGACY);
}

/* ---- network query ---------------------------------------------------------- */
static uint16_t query_seq(void) {
    unsigned n = g_txn;
    tuya_query_network(g_now);
    CHECK(g_txn == n + 9u && g_tx[n + 5] == TUYA_CMD_QUERY_NETWORK && g_tx[n + 6] == 0 && g_tx[n + 7] == 0);
    return (uint16_t)((g_tx[n + 3] << 8) | g_tx[n + 4]);
}

static void network_answer(uint16_t len) {
    const uint8_t joined = TUYA_NETWORK_CONNECTED;
    feed_frame(TUYA_CMD_QUERY_NETWORK, 0, &joined, len);
    tuya_poll(g_now);
}

/* Seq 0 until the module has answered a query (it takes seq 0 for our restart: a lost first
 * query must not hide it), then 1, 2, ...; after 65535 the count goes on at 1, never 0. */
static void test_network_query_seq(void) {
    fresh();
    for (int i = 0; i < 5; i++) {
        CHECK(query_seq() == 0);                 /* no answer yet: still our restart */
    }
    network_answer(0);                           /* no state in it: not an answer */
    CHECK(query_seq() == 0);
    feed_frame(TUYA_CMD_REPORT, 0, NULL, 0);     /* other frames are no answer either */
    feed_frame(TUYA_CMD_PRODUCT_QUERY, 0, NULL, 0);
    tuya_poll(g_now);
    CHECK(query_seq() == 0);
    network_answer(1);
    CHECK(tuya_network_status() == TUYA_NETWORK_CONNECTED);
    CHECK(query_seq() == 1 && query_seq() == 2 && query_seq() == 3);
    network_answer(1);                           /* later answers change nothing */
    CHECK(query_seq() == 4);

    /* One pass over 65536 more: the count wraps to 1, never to 0. */
    g_txn = 0;
    for (uint32_t i = 0; i < 65536u; i++) {
        tuya_query_network(g_now);
    }
    unsigned zeros = 0, frames = 0;
    uint16_t prev = 4;
    int      steps_ok = 1;
    for (unsigned i = 0; i + 9 <= g_txn; i += 9, frames++) {
        uint16_t seq = (uint16_t)((g_tx[i + 3] << 8) | g_tx[i + 4]);
        zeros += seq == 0;
        if (seq != (uint16_t)(prev + 1u) && !(prev == 0xFFFF && seq == 1)) {
            steps_ok = 0;
        }
        prev = seq;
    }
    CHECK(frames == 65536u && zeros == 0 && steps_ok);
    CHECK(prev == 5);                            /* 4 + 65536 with 0 skipped once */
}

static void test_network_query_marks_the_line_busy(void) {
    /* A save (~5 ms stall) does not start while the answer is due. */
    fresh();
    g_now += 5000;
    tuya_poll(g_now);
    CHECK(tuya_rx_quiet_ms(g_now) >= 5000);
    tuya_query_network(g_now);
    CHECK(tuya_rx_quiet_ms(g_now + 20) == 20);
    /* The answer: the network state is taken, and the first one after our start makes us tell
     * our state (the dump). */
    const uint8_t joined = TUYA_NETWORK_CONNECTED;
    feed_frame(TUYA_CMD_QUERY_NETWORK, 0, &joined, 1);
    tuya_poll(g_now + 25);
    CHECK(tuya_network_status() == TUYA_NETWORK_CONNECTED);
    CHECK(tuya_rx_quiet_ms(g_now + 40) == 15);
    CHECK(g_dumps == 1);
}

/* ---- product query 0x01 (a stock Tuya ZTU) and the confirmation ------------- */
static void product_queries(unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        feed_frame(TUYA_CMD_PRODUCT_QUERY, (uint16_t)i, NULL, 0);   /* 55 AA 02 seq 01 00 00 cs */
    }
    tuya_poll(g_now);
}

static void ident_question(void) {
    feed_frame(OTA_CMD_IDENT, 3, NULL, 0);
    tuya_poll(g_now);
}

/* The main loop's call over the first minute: returns the uptime at which the boot word changed
 * (0: it never did). */
static uint32_t confirm_at(void) {
    uint32_t w = g_host_mailbox.boot, c = g_host_mailbox.boot_check;
    for (uint32_t t = 0; t <= 60000u; t += 100u) {
        bootif_poll(t);
        if (g_host_mailbox.boot != w || g_host_mailbox.boot_check != c) {
            return t;
        }
    }
    return 0;
}

static int confirmed_from(uint32_t before) {
    uint32_t w = host_boot_word_now();
    return w == (before & ~BOOT_TRIES_MASK) && g_host_mailbox.boot_check == ~w;
}

static void test_product_query_never_answered(void) {
    fresh();
    product_queries(20);
    feed_frame(TUYA_CMD_PRODUCT_QUERY, 1, k_dp1_on, 5);   /* with data: not the stock query */
    tuya_poll(g_now);
    CHECK(sent_count() == 0 && g_writes == 0 && g_dumps == 0);
}

/* (1) two 0x01, no 0xA3 (a stock Tuya ZTU): confirmed at 30 s, tries 0, every other bit kept. */
static void test_confirm_stock_ztu(void) {
    uint32_t start = BOOT_WORD_FRESH | (2900u << BOOT_PROBE_MS_SHIFT) | BOOT_INFO_STUCK |
                     BOOT_INFO_LINK | 2u;
    fresh();
    host_boot_word(start);
    product_queries(BOOT_CONFIRM_QUERIES);
    CHECK(sent_count() == 0);                    /* still never answered */
    CHECK(confirm_at() == BOOT_CONFIRM_MS);
    CHECK(confirmed_from(start));
}

/* (2) neither: never confirmed (a dead link, or a module that never asks). */
static void test_no_confirm_without_signs(void) {
    fresh();
    host_boot_word(WORD_AT_START);
    CHECK(confirm_at() == 0);
    CHECK(host_boot_word_now() == WORD_AT_START);
}

/* (3) an identity question answered (an own ZTU): confirmed at 30 s. */
static void test_confirm_own_ztu(void) {
    fresh();
    host_boot_word(WORD_AT_START | BOOT_INFO_RESCUE | BOOT_INFO_WRITTEN);
    ident_question();
    CHECK(sent_count() == 1);
    for (uint32_t t = 0; t < BOOT_CONFIRM_MS; t += 1000u) {
        bootif_poll(t);                          /* not before 30 s of uptime */
    }
    CHECK(host_boot_word_now() == (WORD_AT_START | BOOT_INFO_RESCUE | BOOT_INFO_WRITTEN));
    bootif_poll(BOOT_CONFIRM_MS - 1u);
    CHECK(host_boot_word_now() == (WORD_AT_START | BOOT_INFO_RESCUE | BOOT_INFO_WRITTEN));
    bootif_poll(BOOT_CONFIRM_MS);
    CHECK(confirmed_from(WORD_AT_START | BOOT_INFO_RESCUE | BOOT_INFO_WRITTEN));
}

/* An 0xA3 that comes late still confirms, at once: here after 45 s. */
static void test_confirm_late_ident(void) {
    fresh();
    bootif_poll(45000u);
    CHECK(host_boot_word_now() == WORD_AT_START);
    ident_question();
    bootif_poll(45001u);
    CHECK(confirmed_from(WORD_AT_START));
}

/* (4) one 0x01 only, or 0x01 frames with data: not confirmed. */
static void test_no_confirm_one_query(void) {
    fresh();
    host_boot_word(WORD_AT_START);
    product_queries(1);
    feed_frame(TUYA_CMD_PRODUCT_QUERY, 2, k_dp1_on, 5);
    feed_frame(TUYA_CMD_PRODUCT_QUERY, 3, k_dp1_on, 1);
    tuya_poll(g_now);
    CHECK(confirm_at() == 0);
    product_queries(1);                          /* the second one */
    bootif_poll(60001u);
    CHECK(confirmed_from(WORD_AT_START));
}

/* No valid word (the v1 bootloader, random RAM): nothing written. Tries already 0: no write. */
static void test_confirm_needs_a_valid_word(void) {
    fresh();
    host_boot_v1();
    bootif_init();
    g_host_mailbox.boot       = WORD_AT_START;
    g_host_mailbox.boot_check = ~WORD_AT_START ^ 1u;   /* torn */
    ident_question();
    CHECK(confirm_at() == 0);
    CHECK(g_host_mailbox.boot == WORD_AT_START && g_host_mailbox.boot_check == (~WORD_AT_START ^ 1u));
    g_host_mailbox.boot       = 0xA5A5A5A5u;          /* random RAM after a power-up under v1 */
    g_host_mailbox.boot_check = 0x5A5A5A5Au;
    CHECK(confirm_at() == 0 && g_host_mailbox.boot == 0xA5A5A5A5u);
    host_boot_word(BOOT_WORD_FRESH | BOOT_INFO_LINK);  /* valid, tries 0 */
    CHECK(confirm_at() == 0);
}

int main(void) {
    RUN(test_back_to_back_frames);
    RUN(test_stale_header_keeps_the_frame_behind_it);
    RUN(test_stale_headers_in_a_row);
    RUN(test_frame_version_byte);
    RUN(test_damaged_frames_resync);
    RUN(test_offer_newer_byte_accepted);
    RUN(test_offers_declined_byte_rule);
    RUN(test_offer_size_bounds);
    RUN(test_offer_build_rule);
    RUN(test_offer_app_limit);
    RUN(test_ident_and_version_answers);
    RUN(test_build_number);
    RUN(test_boot_info);
    RUN(test_network_query_seq);
    RUN(test_network_query_marks_the_line_busy);
    RUN(test_product_query_never_answered);
    RUN(test_confirm_stock_ztu);
    RUN(test_no_confirm_without_signs);
    RUN(test_confirm_own_ztu);
    RUN(test_confirm_late_ident);
    RUN(test_no_confirm_one_query);
    RUN(test_confirm_needs_a_valid_word);
    const char *build = SIM_ZC ? "SIM" : MEASURE ? "MEASURE" : DIAG_REPORT ? "DIAG" : FW_PREV ? "FW_PREV" : FW_TEST ? "FW_TEST" : "normal";
    if (g_failures) {
        printf("test_tuya (%s build): %d FAILED\n", build, g_failures);
        return 1;
    }
    printf("test_tuya (%s build): all passed\n", build);
    return 0;
}
