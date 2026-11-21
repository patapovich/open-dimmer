/* Host check of the MCU's start bursts (REVIEW-2026-11-18 item 7): at first
 * contact the MCU answers the module's query-all with its whole DP dump and
 * then, back to back, the identity (or version) answer and the ack of a
 * write that was queued meanwhile. The module's UART hands such a burst over
 * as one DMA chunk once the line is idle, and drops a chunk that is larger
 * than its buffer: 508 bytes on a router (512-byte buffer, 4 bytes length).
 * Built from the real tuya.c and dimmer.c, so a new DP in the dump is counted.
 * Run: make test */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bootif.h"
#include "dimmer.h"
#include "ota_proto.h"
#include "phase.h"
#include "store.h"
#include "tuya.h"
#include "uart.h"

#define ZTU_RX_CHUNK_MAX  508u   /* tzs-diag src/telink/hal/uart.c: UART_RX_DMA_BUF_SIZE 512 - 4 */

static int g_failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            g_failures++;                                                \
        }                                                                \
    } while (0)

/* ---- stubs ------------------------------------------------------------------ */
volatile ota_mailbox_t g_host_mailbox;
static uint8_t  g_rxq[256];
static unsigned g_rxh, g_rxt;
static unsigned g_txn, g_tx_frames;

int uart_getc(void) {
    return g_rxt == g_rxh ? -1 : g_rxq[g_rxt++];
}
void uart_putc(uint8_t b) {
    (void)b;
    g_txn++;
}
void uart_flush(void) {
    g_tx_frames++;                 /* tuya_send flushes once per frame */
}
uint32_t uart_dropped(void) {
    return 0;
}
void NVIC_SystemReset(void) {
}
boot_info_t g_boot_info = { 0, 0, APP_LIMIT_LEGACY, 2u, BOOT_FEATURES_V2 };
void bootif_restart(volatile uint32_t *pair, uint32_t value) {
    (void)pair;
    (void)value;
}

uint8_t store_load(uint8_t *data) {
    (void)data;
    return 0;                      /* a new unit: defaults */
}
uint8_t store_save_tries(const uint8_t *data, uint8_t attempts) {
    (void)data;
    (void)attempts;
    return 1;
}
uint8_t store_copy_due(void) {
    return 0;
}
uint32_t store_save_count(void) {
    return 0;
}

void phase_init(void) {
}
void phase_set(uint8_t ch, uint8_t on, uint16_t level) {
}
void phase_set_timed(uint8_t ch, uint8_t on, uint16_t level, uint16_t ms) {
}
void phase_set_range(uint8_t ch, uint16_t min, uint16_t max) {
}
void phase_set_curve(uint8_t ch, uint8_t curve) {
}
void phase_set_comp(uint8_t ch, uint8_t comp4) {
    (void)ch;
    (void)comp4;
}
void phase_hold_start(uint8_t ch, int8_t dir, uint16_t lo, uint16_t hi, uint8_t from_dark) {
}
uint16_t phase_hold_stop(uint8_t ch) {
    return 0;
}
void phase_hold_cancel(uint8_t ch) {
}
void phase_set_profile(uint8_t ch, const dim_profile_t *p) {
}
uint16_t phase_current(uint8_t ch) {
    return 0;
}
void phase_shutdown(void) {
}
void health_soon(uint32_t now_ms) {
    (void)now_ms;
}
void phase_poll(void) {
}
uint8_t phase_save_window(void) {
    return 1;
}

/* ---- helpers ---------------------------------------------------------------- */
static void feed_frame(uint8_t cmd, const uint8_t *d, uint16_t n) {
    uint8_t  f[64];
    unsigned i = 0;
    f[i++] = 0x55;
    f[i++] = 0xAA;
    f[i++] = 0x02;
    f[i++] = 0;
    f[i++] = 1;
    f[i++] = cmd;
    f[i++] = (uint8_t)(n >> 8);
    f[i++] = (uint8_t)n;
    memcpy(f + i, d, n);
    i += n;
    uint8_t s = 0;
    for (unsigned k = 0; k < i; k++) {
        s = (uint8_t)(s + f[k]);
    }
    f[i++] = s;
    g_rxh = g_rxt = 0;
    memcpy(g_rxq, f, i);
    g_rxh = i;
}

static uint32_t g_now = 1000;

/* Bytes the MCU sends in answer to one frame. */
static unsigned answer_bytes(uint8_t cmd, const uint8_t *d, uint16_t n) {
    feed_frame(cmd, d, n);
    g_txn = 0;
    g_now += 100;
    tuya_poll(g_now);
    return g_txn;
}

int main(void) {
    dimmer_init(DIMMER_RESET_POWER);   /* a power-up: the power-on behaviour */
    tuya_init();

    g_txn = g_tx_frames = 0;
    dimmer_report_all();
    unsigned dump = g_txn, dump_frames = g_tx_frames;

    unsigned ident   = answer_bytes(OTA_CMD_IDENT, NULL, 0);
    unsigned version = answer_bytes(TUYA_CMD_VERSION, NULL, 0);
    CHECK(ident == 8u + OTA_IDENT_LEN + 1u && version == 10);   /* identity v2: 19 bytes */

    /* The largest ack of a single-DP write (a write of min also reports max,
     * DP 14 reports 103 and 104 too), over every DP number. */
    unsigned ack_max = 0, ack_level = 0;
    for (unsigned dp = 1; dp < 256; dp++) {
        const uint8_t w[8] = { (uint8_t)dp, TUYA_DP_TYPE_VALUE, 0, 4, 0, 0, 0, 0 };
        unsigned      a    = answer_bytes(TUYA_CMD_WRITE, w, sizeof(w));
        if (a > ack_max) {
            ack_max = a;
        }
        if (dp == 8) {
            ack_level = a;              /* channel 2 level: the 17-byte case of the review */
        }
    }
    CHECK(ack_level == 17);

    unsigned burst_ident   = dump + ident + ack_max;
    unsigned burst_version = dump + version + ack_max;
    printf("contact bursts: dump %u B in %u frames + identity %u B + largest write ack %u B = %u B "
           "(with the version answer %u B) of the module's %u B RX chunk\n",
           dump, dump_frames, ident, ack_max, burst_ident, burst_version, ZTU_RX_CHUNK_MAX);
    CHECK(dump + 19u + 17u <= ZTU_RX_CHUNK_MAX);
    CHECK(burst_ident <= ZTU_RX_CHUNK_MAX);
    CHECK(burst_version <= ZTU_RX_CHUNK_MAX);

    if (g_failures) {
        printf("test_contact_burst: %d FAILED\n", g_failures);
        return 1;
    }
    printf("test_contact_burst: all passed\n");
    return 0;
}
