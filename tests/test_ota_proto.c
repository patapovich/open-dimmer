/* Host test of src/ota_proto.h v2: the layouts and numbers that the released v1 bootloader,
 * the frozen v2 bootloader, the app and the ZTU share (docs/BOOTLOADER.md). The header checks
 * its own struct layouts at compile time; this adds the addresses and values the document
 * names, the boot word operations of each side, and the frame examples of section 5 built from
 * the constants and compared byte for byte. Built twice: with HOST_TEST (the mailbox is a
 * variable, as in the other host tests) and without (the chip address). Run: make test */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "flash_drv.h"
#include "ota_proto.h"

static int g_failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            g_failures++;                                                \
        }                                                                \
    } while (0)

#ifdef HOST_TEST
volatile ota_mailbox_t g_host_mailbox;
#else
_Static_assert(OTA_MAILBOX_ADDR == 0x20000BE0u, "the chip's mailbox address");
#endif

/* Addresses the tools and the frozen code use as literals. */
_Static_assert(APP_BASE + APP_HEADER_OFFSET == 0x080010C0u, "header in flash");
_Static_assert(APP_BASE + APP_HEADER_OFFSET + offsetof(app_header_t, build) == 0x080010CEu, "build at 0xCE");
_Static_assert(BOOT_ID_ADDR == 0x08000010u, "boot_id after the 4-word vector table");
_Static_assert(OTA_MAILBOX_RAM + offsetof(ota_mailbox_t, boot) == 0x20000BF8u &&
               OTA_MAILBOX_RAM + offsetof(ota_mailbox_t, boot_check) == 0x20000BFCu,
               "install.sh and make flash clear these two words");
_Static_assert(BOOT_APP_MAX == 0x3800u && APP_LIMIT_LEGACY == 0x3000u && APP_SIZE_MIN == 0xD0u,
               "section 1");
_Static_assert(OTA_BLOCK_MAX == FLASH_PAGE_BYTES && OTA_ZTU_BLOCK_MAX <= OTA_BLOCK_MAX, "block sizes");

static uint8_t sum(const uint8_t *p, unsigned n) {
    uint8_t s = 0;
    while (n--) {
        s = (uint8_t)(s + *p++);
    }
    return s;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* A Tuya frame (seq 0) with data n bytes; returns its length. */
static unsigned frame(uint8_t *f, uint8_t cmd, const uint8_t *d, unsigned n) {
    const uint8_t h[8] = { 0x55, 0xAA, 0x02, 0, 0, cmd, (uint8_t)(n >> 8), (uint8_t)n };
    memcpy(f, h, 8);
    memcpy(f + 8, d, n);
    f[8 + n] = sum(f, 8 + n);
    return 8 + n + 1;
}

static void test_magics(void) {
    uint32_t le[2] = { APP_MAGIC, BOOT_ID_MAGIC };            /* as they sit in flash (LE) */
    uint8_t  b[8];
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned k = 0; k < 4; k++) {
            b[4 * i + k] = (uint8_t)(le[i] >> (8 * k));
        }
    }
    CHECK(memcmp(b, "APDZODBL", 8) == 0);
    uint8_t be[12];                                           /* the names read as big-endian words */
    put_be32(be, OTA_MAILBOX_MAGIC);
    put_be32(be + 4, OTA_RESTORE_MAGIC);
    put_be32(be + 8, OTA_RESTORED_MAGIC);
    CHECK(memcmp(be, "UPDTRSTRRST1", 12) == 0);
    CHECK(strlen(OTA_IDENT_MAGIC) == OTA_IDENT_AT_KIND);
}

static void test_values(void) {
    /* user decisions of 2026-11-18 */
    CHECK(PROBE_WINDOW_MS == 3000u && RESCUE_WINDOW_MS == 30000u);
    CHECK(LINK_WDG_SILENCE_MS == 30u * 60000u && LINK_WDG_UPTIME_MS == 4u * 3600000u);
    CHECK(LINK_WDG_UPTIME_MS > LINK_WDG_SILENCE_MS && LINK_WDG_SILENCE_MS > BOOT_CONFIRM_MS);
    /* the rest of section 4 */
    CHECK(PROBE_EVERY_MS == 100u && BUTTON_MS == 100u && BUTTON_RELEASE_MS == 5000u);
    CHECK(REQUEST_EVERY_MS == 500u && REJECT_PACE_MS == 50u && GIVE_UP_MS == 10000u);
    CHECK(FRAME_SILENCE_MS == 50u && RESULT_FLUSH_MS == 20u && OTA_ZTU_FIRST_ANSWER_MS == 1500u);
    CHECK(BOOT_CONFIRM_MS == 30000u && BOOT_CONFIRM_QUERIES == 2u);
    /* modes 1..6, each once */
    const unsigned modes[] = { OTA_MODE_ASKED, OTA_MODE_NO_APP, OTA_MODE_PROBE,
                               OTA_MODE_RESCUE, OTA_MODE_BUTTON, OTA_MODE_LINK };
    for (unsigned i = 0; i < 6; i++) {
        CHECK(modes[i] == i + 1u);
    }
    /* header flags: one byte, no overlaps; the bench class is the low three */
    CHECK((APP_FLAG_SIM | APP_FLAG_MEASURE | APP_FLAG_DIAG) == APP_FLAGS_BENCH);
    CHECK((APP_FLAGS_BENCH | APP_FLAG_CONFIRMS | APP_FLAG_RING_HIGH | APP_FLAGS_RESERVED) == 0xFF &&
          APP_FLAGS_BENCH + APP_FLAG_CONFIRMS + APP_FLAG_RING_HIGH + APP_FLAGS_RESERVED == 0xFF);
    /* build numbers */
    CHECK(OTA_BUILD_NONE == 0u && OTA_BUILD_MIN == 256u && OTA_BUILD_MAX == 0xFFFDu);
    CHECK(OTA_BUILD_SIM == 0xFFFEu && OTA_BUILD_NEVER == 0xFFFFu);
    CHECK(OTA_BUILD_FIRST == 300u && OTA_BUILD_FIRST % OTA_BUILD_STEP == 0u && OTA_BUILD_FIRST >= OTA_BUILD_MIN);
    CHECK(OTA_VERSION_SIM == 0xBFu && OTA_VERSION_NEVER == 0xFFu && OTA_VERSION_OURS_MIN == 0x80u);
    /* bootloader identity */
    CHECK(BOOT_VERSION == 3u && BOOT_FEATURES_V2 == 0x0Fu && BOOT_FEAT_LINK == 0x08u);
    CHECK(BOOT_FEAT_HIDDEN == 0x10u && BOOT_FEATURES_V3 == 0x1Fu && BOOT_IMAGE_MAX == 0x6800u &&
          HIDDEN_BASE == 0x08005000u && HIDDEN_SIZE == 0x3000u);
    CHECK(IMAGE_ADDR(0) == APP_BASE && IMAGE_ADDR(0x37FFu) == 0x080047FFu && IMAGE_ADDR(0x3800u) == 0x08005000u &&
          IMAGE_ADDR(BOOT_IMAGE_MAX - 1u) == 0x08007FFFu);
    CHECK(OTA_BOOT_VERSION_V1 == 1u);
    /* frame lengths: v2 = v1 + the appended fields; v1 offsets unchanged */
    CHECK(OTA_REQ_LEN_V1 == 6u && OTA_REQ_LEN == OTA_REQ_AT_INSTALLED + 8u && OTA_REQ_LEN == 16u);
    CHECK(OTA_REQ_AT_OFFSET == 0u && OTA_REQ_AT_MAX_LEN == 4u && OTA_REQ_AT_BOOT == OTA_REQ_LEN_V1);
    CHECK(OTA_NOTIFY_LEN_V1 == 9u && OTA_NOTIFY_AT_BUILD == OTA_NOTIFY_LEN_V1 && OTA_NOTIFY_LEN == 11u);
    CHECK(OTA_NOTIFY_AT_SIZE == 1u && OTA_NOTIFY_AT_CRC == 5u);
    CHECK(OTA_IDENT_LEN_V1 == 7u && OTA_IDENT_AT_BUILD == OTA_IDENT_LEN_V1 && OTA_IDENT_LEN == 10u);
    CHECK(OTA_IDENT_AT_BOOT == 9u && OTA_IDENT_AT_VERSION == 5u && OTA_IDENT_AT_PROTOCOL == 6u);
    CHECK(OTA_RESULT_LEN_V1 == 2u && OTA_RESULT_AT_BUILD == OTA_RESULT_LEN_V1 && OTA_RESULT_LEN == 4u);
    CHECK(OTA_BLOCK_HEAD == 13u && OTA_BLOCK_AT_VERSION == 12u && OTA_HEADER_ANSWER_LEN == 29u);
    CHECK(OTA_RESULT_OK == 0 && OTA_RESULT_BAD_IMAGE == 1 && OTA_RESULT_FLASH == 2 && OTA_NOTIFY_ACCEPT == 0u);
}

/* The boot word, as each side of section 3 handles it. */
static void test_boot_word(void) {
    CHECK(BOOT_WORD_FRESH == 0xB2000000u);
    uint32_t all = BOOT_WORD_FRESH | BOOT_PROBE_MS_MASK | BOOT_INFO_RESCUE | BOOT_INFO_BUTTON |
                   BOOT_INFO_WRITTEN | BOOT_INFO_STUCK | BOOT_PROBE_REQ | BOOT_INFO_LINK |
                   BOOT_RESERVED_BITS | BOOT_TRIES_MASK;
    CHECK((all | 0xFF000000u) == 0xFFFFFFFFu);
    CHECK(BOOT_PROBE_MS_MASK == 0x00FFF000u && BOOT_PROBE_REQ == 0x80u && BOOT_INFO_LINK == 0x40u);
    CHECK(BOOT_TRIES_MASK == 0x0Fu && BOOT_TRIES_MAX == 3u);
    CHECK(((PROBE_WINDOW_MS << BOOT_PROBE_MS_SHIFT) & ~BOOT_PROBE_MS_MASK) == 0u);

    uint32_t w = BOOT_WORD_FRESH;
    CHECK(BOOT_WORD_VALID(w, ~w));
    CHECK(!BOOT_WORD_VALID(w, w));
    CHECK(!BOOT_WORD_VALID(0xB3000000u, ~0xB3000000u));
    CHECK(!BOOT_WORD_VALID(0u, ~0u));

    /* a word after a link probe: probe time 9 ms, LINK, tries 2 */
    w = BOOT_WORD_FRESH | (9u << BOOT_PROBE_MS_SHIFT) | BOOT_INFO_LINK | 2u;
    CHECK((w & BOOT_TRIES_MASK) == 2u);                      /* not 0x42: always masked */
    CHECK((uint8_t)w >= BOOT_TRIES_MAX);                     /* why: an unmasked read would rescue */

    /* app confirmation (section 7): tries 0, every other bit kept */
    uint32_t c = w & ~BOOT_TRIES_MASK;
    CHECK(BOOT_WORD_VALID(c, ~c) && (c & BOOT_TRIES_MASK) == 0u && (c ^ w) == 2u);

    /* app link-watchdog request (section 9, item 11): tries 0 and bit 7, in one word */
    uint32_t r = (w & ~BOOT_TRIES_MASK) | BOOT_PROBE_REQ;
    CHECK(BOOT_WORD_VALID(r, ~r) && (r & BOOT_TRIES_MASK) == 0u && (r & BOOT_PROBE_REQ));
    CHECK((r & ~(BOOT_TRIES_MASK | BOOT_PROBE_REQ)) == (w & ~BOOT_TRIES_MASK));

    /* bootloader (section 4): read and clear bit 7, link probe, +1 at the jump */
    uint32_t link = r & BOOT_PROBE_REQ;
    uint32_t b    = r & ~BOOT_PROBE_REQ;
    CHECK(link && !(b & BOOT_PROBE_REQ));
    b = (b & ~BOOT_PROBE_MS_MASK) | BOOT_INFO_LINK;          /* the link probe's start */
    b |= 9u << BOOT_PROBE_MS_SHIFT;                          /* its answer */
    if ((b & BOOT_TRIES_MASK) < BOOT_TRIES_MAX) {
        b++;
    }
    CHECK(BOOT_WORD_VALID(b, ~b) && (b & BOOT_TRIES_MASK) == 1u && !(b & BOOT_PROBE_REQ));
    CHECK(((b & BOOT_PROBE_MS_MASK) >> BOOT_PROBE_MS_SHIFT) == 9u);
    CHECK((b & 0xFFFFFFu) == 0x009041u);                     /* DP 137 as in the harness log */
    /* the tries never carry into the reserved bits */
    for (uint32_t t = 0; t <= BOOT_TRIES_MAX; t++) {
        uint32_t x = BOOT_WORD_FRESH | BOOT_INFO_LINK | t;
        if ((x & BOOT_TRIES_MASK) < BOOT_TRIES_MAX) {
            x++;
        }
        CHECK((x & ~BOOT_TRIES_MASK) == (BOOT_WORD_FRESH | BOOT_INFO_LINK));
    }
}

/* Section 5: the probe request of an installed build 300 and the answer for a carried 301. */
static void test_frames(void) {
    const app_header_t inst = { APP_MAGIC, 11628u, 0x73B54038u, 0x9A, APP_FLAG_CONFIRMS, 300u };
    uint8_t            d[OTA_REQ_LEN], f[64];
    put_be32(d + OTA_REQ_AT_OFFSET, APP_HEADER_OFFSET);
    d[OTA_REQ_AT_MAX_LEN]     = 0;
    d[OTA_REQ_AT_MAX_LEN + 1] = (uint8_t)sizeof(app_header_t);
    d[OTA_REQ_AT_BOOT]        = 2u;            /* section 5's example: a v2 bootloader */
    d[OTA_REQ_AT_MODE]        = OTA_MODE_PROBE;
    memcpy(d + OTA_REQ_AT_INSTALLED, (const uint8_t *)&inst + 8, 8);   /* as in flash (LE host) */
    static const uint8_t req[] = { 0x55, 0xAA, 0x02, 0x00, 0x00, 0xA1, 0x00, 0x10, 0x00, 0x00, 0x00,
                                   0xC0, 0x00, 0x10, 0x02, 0x03, 0x38, 0x40, 0xB5, 0x73, 0x9A, 0x08,
                                   0x2C, 0x01, 0xF6 };
    unsigned n = frame(f, OTA_CMD_BLOCK, d, sizeof(d));
    CHECK(n == sizeof(req) && memcmp(f, req, n) == 0);
    /* a v1 bootloader's request is the same frame cut after byte 5 of the data */
    CHECK(memcmp(d, req + 8, OTA_REQ_LEN_V1) == 0);
    /* the link probe differs only in the mode byte (and the checksum) */
    d[OTA_REQ_AT_MODE] = OTA_MODE_LINK;
    n = frame(f, OTA_CMD_BLOCK, d, sizeof(d));
    CHECK(n == sizeof(req) && memcmp(f, req, 15) == 0 && f[15] == 0x06 && memcmp(f + 16, req + 16, 8) == 0 &&
          f[24] == (uint8_t)(0xF6 + 3));

    /* the ZTU's answer: offset size crc version + the 16 header bytes (38 bytes in all) */
    const app_header_t carried = { APP_MAGIC, 11628u, 0x11223344u, 0x9B, APP_FLAG_CONFIRMS, 301u };
    uint8_t            a[OTA_HEADER_ANSWER_LEN];
    put_be32(a + OTA_BLOCK_AT_OFFSET, APP_HEADER_OFFSET);
    put_be32(a + OTA_BLOCK_AT_SIZE, carried.size);
    put_be32(a + OTA_BLOCK_AT_CRC, carried.crc);
    a[OTA_BLOCK_AT_VERSION] = carried.version;
    memcpy(a + OTA_BLOCK_AT_DATA, &carried, sizeof(carried));
    n = frame(f, OTA_CMD_BLOCK, a, sizeof(a));
    CHECK(n == 38u && f[7] == 0x1D);
    static const uint8_t size_be[] = { 0x00, 0x00, 0x2D, 0x6C };
    static const uint8_t hdr[]     = { 0x41, 0x50, 0x44, 0x5A, 0x6C, 0x2D, 0x00, 0x00,
                                       0x44, 0x33, 0x22, 0x11, 0x9B, 0x08, 0x2D, 0x01 };
    CHECK(memcmp(f + 8 + OTA_BLOCK_AT_SIZE, size_be, 4) == 0);
    CHECK(memcmp(f + 8 + OTA_BLOCK_AT_DATA, hdr, sizeof(hdr)) == 0);
    /* the bootloader's build read: header bytes 14..15, little-endian */
    const uint8_t *h = f + 8 + OTA_BLOCK_AT_DATA;
    CHECK((uint32_t)(h[14] | h[15] << 8) == 301u && h[13] == APP_FLAG_CONFIRMS && h[12] == 0x9B);
    CHECK(f[n - 1] == sum(f, n - 1));
}

int main(void) {
    test_magics();
    test_values();
    test_boot_word();
    test_frames();
    if (g_failures) {
        printf("test_ota_proto: %d FAILED\n", g_failures);
        return 1;
    }
    printf("test_ota_proto: all passed\n");
    return 0;
}
