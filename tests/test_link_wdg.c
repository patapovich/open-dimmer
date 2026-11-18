/* Host test of the link watchdog (docs/BOOTLOADER.md section 9, item 11; user decision U4):
 * the pure decision (src/link_core.h), its tracking of silence and uptime, and the whole path
 * through the real parser (src/tuya.c: what counts as a frame) and src/bootif.c (when it fires
 * and the word it writes), with the dimmer, the inputs, the gates and the reset as stubs.
 * Streams: an own ZTU (a 0x20 answer every 30 s, contract item 8), a stock Tuya ZTU (0x01 every
 * 1 s, alternating with 1 s of 9600-baud garbage), a dead link (garbage only, or nothing).
 * Built for every build kind: it never fires in SIM, MEASURE or DIAG builds; with
 * LINK_WDG_TEST=1 (bench test B11) after 2 min of silence in a run of 5 min, frames hidden.
 * Run: make test */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "bootif.h"
#include "controls.h"
#include "dimmer.h"
#include "host_boot.h"
#include "link_core.h"
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
#define BENCH  (SIM_ZC || MEASURE || DIAG_REPORT)

static int g_failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            g_failures++;                                                \
        }                                                                \
    } while (0)

#define MINUTE   60000u
#define HOUR     (60u * MINUTE)

/* ---- stubs: UART, dimmer, inputs, gates, reset ------------------------------ */
volatile ota_mailbox_t g_host_mailbox;
static uint8_t  g_rxq[1u << 16];
static uint16_t g_rxh, g_rxt;              /* a ring: wraps by itself */
static unsigned g_tx_bytes;
static uint8_t  g_dark = 1, g_idle = 1;
static int      g_step;                     /* restart path: 1 shutdown, 2 prepared, 3 reset */
static unsigned g_resets;
static uint32_t g_word_at_prepare, g_word_at_reset, g_check_at_reset;

int uart_getc(void) {
    return g_rxt == g_rxh ? -1 : g_rxq[g_rxt++];
}
void uart_putc(uint8_t b) {
    (void)b;
    g_tx_bytes++;
}
void uart_flush(void) {
}
uint32_t uart_dropped(void) {
    return 0;
}
uint8_t dimmer_write_dp(uint8_t dpid, uint8_t type, const uint8_t *v, uint16_t len) {
    (void)dpid;
    (void)type;
    (void)v;
    (void)len;
    return 1;
}
void dimmer_report_dp(uint8_t dpid) {
    (void)dpid;
}
uint8_t dimmer_sibling_dp(uint8_t dpid, uint8_t n) {
    (void)dpid;
    (void)n;
    return 0;
}
void dimmer_report_all(void) {
}
uint8_t dimmer_dark(void) {
    return g_dark;
}
uint8_t controls_idle(void) {
    return g_idle;
}
void phase_shutdown(void) {
    CHECK(g_step == 0);
    g_step = 1;                             /* gates first */
}
void dimmer_prepare_restart(void) {
    CHECK(g_step == 1);
    g_step            = 2;
    g_word_at_prepare = g_host_mailbox.boot;
}
void NVIC_SystemReset(void) {
    CHECK(g_step == 2);
    g_step           = 3;
    g_resets++;
    g_word_at_reset  = g_host_mailbox.boot;
    g_check_at_reset = g_host_mailbox.boot_check;
}

/* ---- the module's side ----------------------------------------------------- */
static void feed_byte(uint8_t b) {
    g_rxq[g_rxh++] = b;
}

static void feed_frame(uint8_t cmd, uint16_t seq, const uint8_t *d, uint16_t n) {
    uint8_t  h[8] = { 0x55, 0xAA, 0x02, (uint8_t)(seq >> 8), (uint8_t)seq, cmd, (uint8_t)(n >> 8),
                      (uint8_t)n };
    uint8_t  s    = 0;
    for (unsigned i = 0; i < 8; i++) {
        s = (uint8_t)(s + h[i]);
        feed_byte(h[i]);
    }
    for (unsigned i = 0; i < n; i++) {
        s = (uint8_t)(s + d[i]);
        feed_byte(d[i]);
    }
    feed_byte(s);
}

static uint32_t g_rand = 12345u;
static uint8_t  rnd(void) {
    g_rand = g_rand * 1103515245u + 12345u;
    return (uint8_t)(g_rand >> 16);
}

/* Bytes that form no frame: 9600-baud-like noise, and now and then a frame header whose
 * checksum or version is wrong. */
static void feed_garbage(unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if ((rnd() & 63u) == 0) {
            /* A 0x01 query whose checksum is off (or whose version byte is not 0x02). */
            uint8_t bad[9] = { 0x55, 0xAA, (uint8_t)(rnd() & 1u ? 0x02 : 0x03), 0, 1,
                               TUYA_CMD_PRODUCT_QUERY, 0, 0, 0 };
            uint8_t sum    = 0;
            for (unsigned k = 0; k < 8; k++) {
                sum = (uint8_t)(sum + bad[k]);
            }
            bad[8] = (uint8_t)(sum + 1u + rnd() % 255u);
            for (unsigned k = 0; k < 9; k++) {
                feed_byte(bad[k]);
            }
        } else {
            feed_byte(rnd());
        }
    }
}

enum { ZTU_OWN, ZTU_STOCK, ZTU_GARBAGE, ZTU_SILENT };

/* One 100 ms main-loop slice of model time t (ms since the start of the run). */
static void ztu_slice(int kind, uint32_t t) {
    static const uint8_t joined = TUYA_NETWORK_CONNECTED;
    switch (kind) {
    case ZTU_OWN:                               /* the 0x20 answer, every 30 s */
        if (t % 30000u == 0) {
            feed_frame(TUYA_CMD_QUERY_NETWORK, (uint16_t)(t / 30000u), &joined, 1);
        }
        break;
    case ZTU_STOCK:                             /* 0x01 every 1 s, a garbage phase every other 1 s */
        if (t % 1000u == 0) {
            feed_frame(TUYA_CMD_PRODUCT_QUERY, 0, NULL, 0);
        }
        if ((t / 1000u) & 1u) {
            feed_garbage(96);                   /* ~1 byte per 1.04 ms */
        }
        break;
    case ZTU_GARBAGE:
        feed_garbage(96);
        break;
    default:
        break;
    }
}

/* ---- runs ------------------------------------------------------------------ */
#define WORD_AT_START (BOOT_WORD_FRESH | (300u << BOOT_PROBE_MS_SHIFT) | 1u)

/* A fresh run (a forked process: tuya.c and bootif.c start zeroed as after a reset). */
static void run_start(int v2, uint8_t features, uint32_t word) {
    memset((void *)&g_host_mailbox, 0, sizeof(g_host_mailbox));
    if (v2) {
        host_boot_v2(features, BOOT_APP_MAX);
    } else {
        host_boot_v1();
    }
    host_boot_word(word);
    bootif_init();
    tuya_init();
}

/* The main loop from model time `from` to `to` in 100 ms passes, the module sending `kind`,
 * lit (not dark) in [lit_from, lit_to). Returns the time of the restart, or 0. */
static uint32_t run_loop(int kind, uint32_t from, uint32_t to, uint32_t lit_from, uint32_t lit_to) {
    for (uint32_t t = from; t < to; t += 100u) {
        ztu_slice(kind, t);
        g_dark = !(t >= lit_from && t < lit_to);
        tuya_poll(t);
        bootif_poll(t);
        if (g_resets) {
            return t;
        }
    }
    return 0;
}

static void in_child(void (*test)(void), const char *name) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        g_failures = 0;
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
#define RUN(t) in_child(t, #t)

/* ---- the decision, a pure function ------------------------------------------- */
static void test_decision(void) {
    link_track_t t   = { 0, 0, 1 };
    uint32_t     now = LINK_SILENCE_MS;
    uint32_t     w   = WORD_AT_START;
    CHECK(link_restart_due(&t, now, 1, APP_FLAG_CONFIRMS, BOOT_FEATURES_V2, 1));
    CHECK(link_restart_due(&t, now, 1, 0, BOOT_FEAT_LINK, BOOT_WORD_VALID(w, ~w)));
    CHECK(link_restart_due(&t, now, 1, APP_FLAG_CONFIRMS | APP_FLAG_RING_HIGH, BOOT_FEAT_LINK, 1));
    CHECK(!link_restart_due(&t, now - 1u, 1, APP_FLAG_CONFIRMS, BOOT_FEATURES_V2, 1));   /* silence */
    CHECK(!link_restart_due(&t, now, 0, APP_FLAG_CONFIRMS, BOOT_FEATURES_V2, 1));        /* lit or pressed */
    CHECK(!link_restart_due(&t, now, 1, APP_FLAG_CONFIRMS | APP_FLAG_SIM, BOOT_FEATURES_V2, 1));
    CHECK(!link_restart_due(&t, now, 1, APP_FLAG_CONFIRMS | APP_FLAG_MEASURE, BOOT_FEATURES_V2, 1));
    CHECK(!link_restart_due(&t, now, 1, APP_FLAG_CONFIRMS | APP_FLAG_DIAG, BOOT_FEATURES_V2, 1));
    CHECK(!link_restart_due(&t, now, 1, APP_FLAG_CONFIRMS, BOOT_FEATURES_V2 & ~BOOT_FEAT_LINK, 1));
    CHECK(!link_restart_due(&t, now, 1, APP_FLAG_CONFIRMS, 0, 1));                       /* v1 */
    CHECK(!link_restart_due(&t, now, 1, APP_FLAG_CONFIRMS, BOOT_FEATURES_V2,
                            BOOT_WORD_VALID(w, ~w ^ 1u)));                                /* torn */
    t.aged = 0;
    CHECK(!link_restart_due(&t, now, 1, APP_FLAG_CONFIRMS, BOOT_FEATURES_V2, 1));        /* young */
}

/* Silence and uptime from the tracker: exactly 30 min after the last frame in a run of 4 h,
 * not before 4 h, and the ms counter's wrap after 49.7 days changes nothing. */
static uint8_t due_at(link_track_t *t, uint32_t now, uint32_t frames) {
    link_track(t, now, frames);
    return link_restart_due(t, now, 1, APP_FLAG_CONFIRMS, BOOT_FEATURES_V2, 1);
}

static void test_tracker(void) {
    link_track_t t      = { 0 };
    uint32_t     frames = 0, now;
    for (now = 0; now < LINK_UPTIME_MS; now += 1000u) {          /* frames every 30 s */
        if (now % 30000u == 0) {
            frames++;
        }
        CHECK(!due_at(&t, now, frames));
    }
    uint32_t last = now - 1000u - (now - 1000u) % 30000u;        /* the last frame's pass */
    for (; now < last + LINK_SILENCE_MS; now += 1000u) {
        CHECK(!due_at(&t, now, frames));
    }
    CHECK(due_at(&t, last + LINK_SILENCE_MS - 1u, frames) == 0);
    CHECK(due_at(&t, last + LINK_SILENCE_MS, frames) == 1);       /* exactly 30 min */
    CHECK(due_at(&t, last + LINK_SILENCE_MS + 1u, frames + 1u) == 0);   /* a frame: again */

    /* No frame at all from the start: silence since the start, but not before 4 h. */
    link_track_t u = { 0 };
    for (now = 0; now < LINK_UPTIME_MS; now += 500u) {
        CHECK(!due_at(&u, now, 0));
    }
    CHECK(due_at(&u, LINK_UPTIME_MS, 0));

    /* 50 days with a frame every 30 s; frames stop just after the counter wrapped. */
    link_track_t v = { 0 };
    uint64_t     m;
    frames = 0;
    for (m = 0; m < 4294967296ull + 120000u; m += 30000u) {
        CHECK(!due_at(&v, (uint32_t)m, ++frames));
    }
    uint32_t heard = (uint32_t)(m - 30000u);
    CHECK(heard < LINK_UPTIME_MS);                                 /* small again after the wrap */
    CHECK(!due_at(&v, heard + LINK_SILENCE_MS - 1u, frames));
    CHECK(due_at(&v, heard + LINK_SILENCE_MS, frames));            /* not 4 h later */
}

/* ---- through tuya.c and bootif.c ------------------------------------------- */
#if !BENCH && !LINK_WDG_TEST
static int g_pipe[2];                       /* a result from a forked run */

/* A working ZTU of either kind never lets it fire, lit or dark, over 24 h. */
static void own_ztu_day(void) {
    run_start(1, BOOT_FEATURES_V2, WORD_AT_START);
    CHECK(run_loop(ZTU_OWN, 0, 24u * HOUR, 0, 0) == 0 && g_resets == 0);
    CHECK(g_tuya_frames == 24u * 120u);
}

static void stock_ztu_day(void) {
    run_start(1, BOOT_FEATURES_V2, WORD_AT_START);
    CHECK(run_loop(ZTU_STOCK, 0, 24u * HOUR, 0, 0) == 0 && g_resets == 0);
    CHECK(g_tuya_frames == 24u * 3600u);          /* the 0x01 frames only: garbage is no frame */
    CHECK(g_tuya_confirm >= BOOT_CONFIRM_QUERIES);
}

/* The stream stops at 5 h (the module dies): 30 min later the restart, with exactly the request
 * word, after the gates went low and the state was kept, and nothing else in the mailbox. */
static void stock_ztu_dies(void) {
    uint32_t w = BOOT_WORD_FRESH | BOOT_INFO_LINK | BOOT_INFO_WRITTEN | (12u << BOOT_PROBE_MS_SHIFT) | 3u;
    run_start(1, BOOT_FEATURES_V2, w);
    g_host_mailbox.expect_crc    = 0x1234u;
    g_host_mailbox.restore       = 0x5678u;
    g_host_mailbox.restore_check = 0x9ABCu;
    g_host_mailbox.restored      = OTA_RESTORED_MAGIC;
    CHECK(run_loop(ZTU_STOCK, 0, 5u * HOUR, 0, 0) == 0);
    uint32_t last = 5u * HOUR - 1000u;            /* its last 0x01 */
    uint32_t at   = run_loop(ZTU_GARBAGE, 5u * HOUR, 24u * HOUR, 0, 0);
    CHECK(at == last + LINK_SILENCE_MS);
    /* The 0x01 queries confirmed the run at 30 s (tries 0); the word is written last. */
    CHECK(g_resets == 1 && g_step == 3 && g_word_at_prepare == (w & ~BOOT_TRIES_MASK));
    CHECK(g_word_at_reset == ((w & ~BOOT_TRIES_MASK) | BOOT_PROBE_REQ) && g_check_at_reset == ~g_word_at_reset);
    CHECK((g_word_at_reset & 0xFFu) == 0xC0u);    /* tries 0, request, LINK kept */
    CHECK(g_host_mailbox.magic == 0 && g_host_mailbox.magic_inv == 0);   /* no asked update */
    CHECK(g_host_mailbox.expect_crc == 0x1234u && g_host_mailbox.restore == 0x5678u &&
          g_host_mailbox.restore_check == 0x9ABCu);
    CHECK(g_host_mailbox.restored == OTA_RESTORED_MAGIC);   /* dimmer_prepare_restart's (a stub) */
}

/* Any well-formed frame starts the silence again, whatever its command; garbage never: 3 h 50 min
 * of garbage, one frame of a command nobody knows, garbage again: the restart comes 30 min after
 * that frame, not at 4 h. */
static void any_frame_counts(void) {
    const uint32_t at = 3u * HOUR + 50u * MINUTE;
    run_start(1, BOOT_FEATURES_V2, WORD_AT_START);
    CHECK(run_loop(ZTU_GARBAGE, 0, at, 0, 0) == 0);
    CHECK(run_loop(ZTU_SILENT, at, at + 1000u, 0, 0) == 0);   /* idle line: half frames dropped */
    CHECK(g_tuya_frames == 0);
    const uint8_t d[2] = { 1, 2 };
    feed_frame(0x77, 9, d, sizeof(d));
    CHECK(run_loop(ZTU_GARBAGE, at + 1000u, 24u * HOUR, 0, 0) == at + 1000u + LINK_SILENCE_MS);
    CHECK(g_tuya_frames == 1);
}

static void test_streams(void) {
    RUN(own_ztu_day);
    RUN(stock_ztu_day);
    RUN(stock_ztu_dies);
    RUN(any_frame_counts);
}

/* A dead link: one restart per run, at 4 h at the earliest (every restart begins a new run),
 * and only while dark: over 24 model hours, with the light on from 9 h to 13 h. */
static uint32_t g_day_from, g_lit_from, g_lit_to;
static void dead_link_run(void) {
    run_start(1, BOOT_FEATURES_V2, WORD_AT_START);
    uint32_t lit_from = g_lit_from > g_day_from ? g_lit_from - g_day_from : 0;
    uint32_t lit_to   = g_lit_to > g_day_from ? g_lit_to - g_day_from : 0;
    uint32_t at       = run_loop(ZTU_SILENT, 0, 24u * HOUR - g_day_from, lit_from, lit_to);
    CHECK(write(g_pipe[1], &at, sizeof(at)) == (ssize_t)sizeof(at));
}

static void test_dead_link_day(void) {
    uint32_t restarts[16], n = 0;
    g_lit_from = 9u * HOUR;
    g_lit_to   = 13u * HOUR;
    for (g_day_from = 0; g_day_from < 24u * HOUR && n < 16;) {
        CHECK(pipe(g_pipe) == 0);
        in_child(dead_link_run, "dead_link_run");
        uint32_t at = 0;
        CHECK(read(g_pipe[0], &at, sizeof(at)) == (ssize_t)sizeof(at));
        close(g_pipe[0]);
        close(g_pipe[1]);
        if (!at) {
            break;                                /* no restart left in the day */
        }
        restarts[n++] = g_day_from + at;
        g_day_from += at + 100u;                  /* the new run starts after the reset */
    }
    /* 4, 8 h; 12 h lies in the lit time: 13 h; then 17, 21 h. */
    CHECK(n == 5);
    CHECK(n >= 5 && restarts[0] == 4u * HOUR && restarts[1] == 8u * HOUR + 100u &&
          restarts[2] == 13u * HOUR && restarts[3] == 17u * HOUR + 100u &&
          restarts[4] == 21u * HOUR + 200u);
    for (uint32_t i = 1; i < n; i++) {
        CHECK(restarts[i] - restarts[i - 1] >= LINK_UPTIME_MS);
    }
    for (uint32_t i = 0; i < n; i++) {
        CHECK(restarts[i] < g_lit_from || restarts[i] >= g_lit_to);
    }
}

/* Never with a busy input, only with the bootloader's feature, a valid word, a v2 boot_id. */
static void pressed_never(void) {
    run_start(1, BOOT_FEATURES_V2, WORD_AT_START);
    g_idle = 0;
    CHECK(run_loop(ZTU_SILENT, 0, 12u * HOUR, 0, 0) == 0);
    g_idle = 1;
    CHECK(run_loop(ZTU_SILENT, 12u * HOUR, 13u * HOUR, 0, 0) == 12u * HOUR);
}
static void v1_never(void) {
    run_start(0, 0, WORD_AT_START);              /* even with a word that looks valid */
    CHECK(run_loop(ZTU_SILENT, 0, 24u * HOUR, 0, 0) == 0);
    CHECK(host_boot_word_now() == WORD_AT_START);
}
static void no_feature_never(void) {
    run_start(1, BOOT_FEATURES_V2 & ~BOOT_FEAT_LINK, WORD_AT_START);
    CHECK(run_loop(ZTU_SILENT, 0, 24u * HOUR, 0, 0) == 0);
}
static void invalid_word_never(void) {
    run_start(1, BOOT_FEATURES_V2, WORD_AT_START);
    g_host_mailbox.boot_check ^= 0x100u;
    CHECK(run_loop(ZTU_SILENT, 0, 24u * HOUR, 0, 0) == 0);
    CHECK(g_host_mailbox.boot == WORD_AT_START);
}

/* The restart after it: tries 0 + the bootloader's + 1 = 1, so link restarts never add up to a
 * counter rescue, however many come (10 runs in a row, each a new process as after a reset). */
static uint32_t g_run_word;
static void one_link_run(void) {
    run_start(1, BOOT_FEATURES_V2, g_run_word);
    CHECK(run_loop(ZTU_SILENT, 0, 5u * HOUR, 0, 0) == LINK_UPTIME_MS);
    CHECK(write(g_pipe[1], &g_word_at_reset, sizeof(g_word_at_reset)) == (ssize_t)sizeof(g_word_at_reset));
}

static void test_counter_never_moves(void) {
    g_run_word = WORD_AT_START | 2u;                /* even from 3 unconfirmed boots */
    for (int i = 0; i < 10; i++) {
        uint32_t r = 0;
        CHECK(pipe(g_pipe) == 0);
        in_child(one_link_run, "one_link_run");
        CHECK(read(g_pipe[0], &r, sizeof(r)) == (ssize_t)sizeof(r));
        close(g_pipe[0]);
        close(g_pipe[1]);
        CHECK((r & BOOT_TRIES_MASK) == 0 && (r & BOOT_PROBE_REQ) && (r >> BOOT_TAG_SHIFT) == BOOT_TAG);
        /* The v2 bootloader: bit 7 read and cleared, the link probe (LINK, the probe time
         * cleared, here no answer), tries + 1 at the jump. */
        g_run_word = ((r & ~(BOOT_PROBE_REQ | BOOT_PROBE_MS_MASK)) | BOOT_INFO_LINK) + 1u;
        CHECK((g_run_word & BOOT_TRIES_MASK) == 1u);
    }
}

static void test_arming(void) {
    RUN(pressed_never);
    RUN(v1_never);
    RUN(no_feature_never);
    RUN(invalid_word_never);
    test_counter_never_moves();
}

#elif LINK_WDG_TEST

/* Bench test B11: thresholds cut to 2 min / 5 min, frames hidden from the watchdog only. */
static void b11_fires_at_5_min(void) {
    run_start(1, BOOT_FEATURES_V2, WORD_AT_START);
    CHECK(LINK_SILENCE_MS == 2u * MINUTE && LINK_UPTIME_MS == 5u * MINUTE);
    CHECK(run_loop(ZTU_OWN, 0, HOUR, 0, 0) == 5u * MINUTE);
    CHECK(g_tuya_frames == 10u + 1u);             /* the frames were handled all the same */
}
static void b11_never_lit(void) {
    run_start(1, BOOT_FEATURES_V2, WORD_AT_START);
    CHECK(run_loop(ZTU_OWN, 0, HOUR, 0, 30u * MINUTE) == 30u * MINUTE);
}
static void test_b11(void) {
    RUN(b11_fires_at_5_min);
    RUN(b11_never_lit);
}

#else

/* Bench builds: compiled out, a dead link never restarts them (the bootloader skips their probe). */
static void bench_never(void) {
    run_start(1, BOOT_FEATURES_V2, WORD_AT_START);
    CHECK(run_loop(ZTU_SILENT, 0, 24u * HOUR, 0, 0) == 0 && g_resets == 0);
    run_start(1, BOOT_FEATURES_V2, WORD_AT_START);
    CHECK(run_loop(ZTU_GARBAGE, 0, 6u * HOUR, 0, 0) == 0 && g_resets == 0);
}

#endif

int main(void) {
    RUN(test_decision);
    RUN(test_tracker);
#if !BENCH && !LINK_WDG_TEST
    test_streams();
    test_dead_link_day();
    test_arming();
#elif LINK_WDG_TEST
    test_b11();
#else
    RUN(bench_never);
#endif
    const char *build = SIM_ZC ? "SIM" : MEASURE ? "MEASURE" : DIAG_REPORT ? "DIAG"
                      : LINK_WDG_TEST ? "LINK_WDG_TEST" : "normal";
    if (g_failures) {
        printf("test_link_wdg (%s build): %d FAILED\n", build, g_failures);
        return 1;
    }
    printf("test_link_wdg (%s build): all passed\n", build);
    return 0;
}
