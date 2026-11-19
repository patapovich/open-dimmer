/* PY32 firmware update over the Tuya UART, protocol v2 (docs/BOOTLOADER.md).
 *
 * The Zigbee module (ZTU) carries the app image inside its own OTA image and serves it to the
 * MCU block by block. Shared by the app (src/) and the bootloader (boot/); the ZTU (fork
 * src/zigbee/tuya_mcu_ota.*) keeps its own copy of these numbers, pinned by its tests.
 *
 * Who reads what, in the comments below:
 *   v1 boot   the bootloader released with 2.1.x (still on the spare until its SWD re-flash)
 *   v2 boot   the phase-3 bootloader: FROZEN once installed, replaced only over SWD
 *   2.1.x app / v2 app   the app before / from release R (build 300) on
 *   ZTU p1 / ZTU p3      own Zigbee firmware of phase 1 / of phase 3 and later
 * Everything a v1 or v2 bootloader reads keeps its offset and meaning for ever. Unknown bytes
 * after the known ones are ignored by every reader (old peers read only the bytes they know).
 *
 * Numbers inside Tuya frames are big-endian; the app header and the bootloader identity are
 * little-endian, as they sit in flash. Frames: 55 AA 02 <seq:2> <cmd> <len:2> <data> <sum>.
 *
 * Commands (ours; Tuya's own MCU OTA is 0x0C..0x0E):
 *   0xA0 ZTU -> app    offer: version(1) size(4) crc(4) [build(2)]           OTA_NOTIFY_*
 *        app -> ZTU    answer: status(1), 0 = accepted, restarting into the bootloader
 *   0xA1 boot -> ZTU   block request: offset(4) max_len(2) [boot(1) mode(1) installed(8)]
 *        ZTU -> boot   block: offset(4) size(4) crc(4) version(1) data(n)    OTA_REQ_*, OTA_BLOCK_*
 *   0xA2 boot -> ZTU   result: status(1) version(1) [build(2)]               OTA_RESULT_*
 *   0xA3 ZTU -> app    "who are you" (no data); only our app answers:
 *        app -> ZTU    "ZDMS"(4) kind(1) version(1) protocol(1) [build(2) boot(1)]   OTA_IDENT_*
 *                      A stock MCU never answers, whatever its version byte: that is how the
 *                      ZTU tells our firmware from the stock one.
 * [..] = v2 only. The bootloader answers nothing: it only asks (0xA1) and reports (0xA2). */
#ifndef OTA_PROTO_H
#define OTA_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* ---- flash layout (PY32F002A: 20 KB, 128-byte pages, 4 KB sectors) --------------------------- */

#define BOOT_BASE             0x08000000u   /* sector 0: the bootloader alone (SWD only) */
#define BOOT_SIZE             0x1000u
#define BOOT_ID_ADDR          (BOOT_BASE + 0x10u)   /* boot_id_t, right after the 4-word vector table */
#define APP_BASE              0x08001000u   /* app image: vectors, header at +APP_HEADER_OFFSET, code */
#define APP_HEADER_OFFSET     0xC0u         /* after the 48-entry vector table */
#define BOOT_APP_MAX          0x3800u       /* 14 KB: the v2 bootloader's own ceiling (no valid app) */
#define APP_LIMIT_LEGACY      0x3000u       /* 12 KB: an app whose settings ring is at 0x08004000, and
                                               the v1 bootloader's ceiling (replaces APP_MAX_SIZE) */
#define SETTINGS_BASE_LEGACY  (APP_BASE + APP_LIMIT_LEGACY)   /* 0x08004000: ring of 32 pages */
#define SETTINGS_BASE_HIGH    (APP_BASE + BOOT_APP_MAX)       /* 0x08004800: ring of 16 pages (APP_FLAG_RING_HIGH) */
#define FLASH_TOP             0x08005000u   /* end of the 20 KB flash */

/* ---- app header (16 bytes at image offset 0xC0, flash 0x080010C0) ---------------------------- */

#define APP_MAGIC             0x5A445041u   /* "APDZ" */

typedef struct {
    uint32_t magic;     /* APP_MAGIC. Read by: v1 boot, v2 boot, ZTU p1/p3, tools */
    uint32_t size;      /* image bytes from APP_BASE, filled in by tools/mkimage.py. Same readers */
    uint32_t crc;       /* CRC-32 (zlib) of the image with this field read as 0; never 0. Same readers */
    uint8_t  version;   /* Tuya packed: 2 bits major, 2 minor, 4 patch (OTA_VERSION_*).
                           Read by: ZTU p1/p3 (offers, served in every block answer), v2 boot only to
                           echo it (request bytes 12, result byte 1); display, converter */
    uint8_t  flags;     /* APP_FLAG_*. Read by: v2 boot (bench, CONFIRMS, RING_HIGH), ZTU p1 (SIM,
                           MEASURE), ZTU p3 (all), tools. The v1 boot ignores it */
    uint16_t build;     /* OTA_BUILD_*, 0 = none (2.1.x and older; their bytes were reserved {0, 0}).
                           Read by: v2 boot (probe: newer only), ZTU p3 (offers), v2 app, tools */
} app_header_t;

#define APP_SIZE_MIN          (APP_HEADER_OFFSET + (uint32_t)sizeof(app_header_t))   /* 0xD0 */

#define APP_FLAG_SIM          0x01    /* bench build (virtual zero cross): never on mains */
#define APP_FLAG_MEASURE      0x02    /* measurement build: outputs always off */
#define APP_FLAG_DIAG         0x04    /* diagnostics build: takes no update */
#define APP_FLAGS_BENCH       0x07    /* the bench class: never carried, never taken by the v2 boot in any
                                         mode, never counted, no probe; leaves over SWD or a button rescue */
#define APP_FLAG_CONFIRMS     0x08    /* v2 apps: confirms its runs in the boot word (counted when not bench) */
#define APP_FLAG_RING_HIGH    0x10    /* settings ring at SETTINGS_BASE_HIGH: images up to BOOT_APP_MAX fit */
#define APP_FLAGS_RESERVED    0xE0    /* 0; the v2 boot ignores them, mkimage.py refuses them */

/* Version byte (header, 0xA0 byte 0, 0xA3 byte 5, 0x0B answer). Major >= 2 for ever: the ZTU asks
 * 0xA3 only from major 2, and the converter takes major < 2 for a stock MCU (1.0.7 = 0x47). */
#define OTA_VERSION_OURS_MIN  0x80u   /* 2.0.0 */
#define OTA_VERSION_SIM       0xBFu   /* SIM builds only (src/tuya.h) */
#define OTA_VERSION_NEVER     0xFFu   /* never made: no byte-rule offer could follow it */

/* Build number (header 0xCE..0xCF LE, 0xA0 bytes 9-10 BE, 0xA3 bytes 7-8 BE, 0xA2 bytes 2-3 BE) */
#define OTA_BUILD_NONE        0u        /* 2.1.x: valid and servable, never newer in a build compare */
#define OTA_BUILD_MIN         256u      /* 1..255: never (mkimage.py, embed_ztu.py, ZTU p3 refuse) */
#define OTA_BUILD_MAX         0xFFFDu
#define OTA_BUILD_SIM         0xFFFEu   /* SIM builds: nothing is newer, so nothing is offered to them */
#define OTA_BUILD_NEVER       0xFFFFu
#define OTA_BUILD_FIRST       300u      /* release R */
#define OTA_BUILD_STEP        100u      /* release to release; test builds take the previous release + 1..99 */

/* ---- bootloader identity (16 bytes at BOOT_ID_ADDR; v2 boot only) ---------------------------- */

#define BOOT_ID_MAGIC         0x4C42444Fu   /* "ODBL". The v1 boot has code there (80 22 03 4b): no match */
#define BOOT_VERSION          2u

typedef struct {
    uint32_t magic;      /* BOOT_ID_MAGIC. Read by: v2 app (app_limit, 0xA3 byte 9, link watchdog),
                            tools/swd/install.sh, mkimage.py --full */
    uint8_t  version;    /* BOOT_VERSION */
    uint8_t  features;   /* BOOT_FEAT_* */
    uint16_t reserved;   /* 0xFFFF */
    uint32_t app_max;    /* BOOT_APP_MAX */
    uint32_t mailbox;    /* OTA_MAILBOX_RAM */
} boot_id_t;

#define BOOT_FEAT_TRIES       0x01u   /* try counter and counter rescue (APP_FLAG_CONFIRMS) */
#define BOOT_FEAT_PROBE       0x02u   /* power-on probe */
#define BOOT_FEAT_BUTTON      0x04u   /* button rescue */
#define BOOT_FEAT_LINK        0x08u   /* honours BOOT_PROBE_REQ (the app's link watchdog) */
#define BOOT_FEATURES_V2      (BOOT_FEAT_TRIES | BOOT_FEAT_PROBE | BOOT_FEAT_BUTTON | BOOT_FEAT_LINK)
#define OTA_BOOT_VERSION_V1   1u      /* what the app reports (0xA3 byte 9) without a valid boot_id */

/* ---- frozen ZTU contract (docs/BOOTLOADER.md section 10; ZTU side: fork tuya_mcu_ota.h) -------
 * What the frozen v2 bootloader needs from every future ZTU, checked for every ZTU release on the
 * spare (B2) and pinned by the fork's tests:
 *  1. 0xA1 v1 semantics for ever: a request is offset(4) max_len(2) + any further bytes, which the
 *     serving rule ignores; the answer is offset size crc version + up to min(max_len, 128) image
 *     bytes, without state, for any offset. Serving is gated only by the image check and the failure
 *     pause, never by build, version or the request's mode.
 *  2. Robust: no crash, no stall on any request a bootloader can send (offset 0..0xFFFFFFFF, max_len
 *     0..0xFFFF, 6..16 bytes, up to one per 50 ms, for days).
 *  3. Timely: the first header answer within OTA_ZTU_FIRST_ANSWER_MS of power-up, also on the first
 *     boot after a ZTU OTA. An earlier UART start only with the first-contact timer at the end of the
 *     full init (>= 2000 ms) and an answer-only (0xA1) link before it.
 *  4. Identity by class of MCU (the major only from a 0x0B answer in this ZTU run): (a) identified as
 *     Open Dimmer: 0xA3 again after every seq-0 network query, offset-0 request and OK result;
 *     (b) major >= 2, not identified: 4 questions per version answer + 2 late, at most 3 treated
 *     restarts per ZTU run; (c) major < 2 or no 0x0B answer: never asked.
 *  5. Results: 0xA2 status 0 = written and valid; anything else counts toward the failure pause.
 * Not needed by the frozen code, pinned the same way:
 *  6. Factory MCUs (not identified as Open Dimmer): only the phase-1 command set, one queued frame
 *     in flight, <= 29 bytes per frame, <= 23 bytes per burst (< 2 ms apart); frames over 29 bytes
 *     only as the direct reply to an 0xA1 request. The basis: docs/stock-firmware-analysis.md,
 *     "Stock MCU parser limits" (23-byte ISR ring; 31-byte parse buffer, so a 32..38-byte frame
 *     wedges RX until a power cut; read from the WiFi 1.0.3, inferred for the Zigbee 1.0.7).
 *  7. No 0x01 (product query) to any MCU: the v2 app takes 0x01 without 0xA3 as a stock Tuya ZTU.
 *  8. Heard: an MCU identified as Open Dimmer gets a well-formed frame at least every 30 min; in
 *     practice every 0x20 network query (the app sends one every 30 s) is answered, as in phase 1.
 *     The v2 app's link watchdog takes LINK_WDG_SILENCE_MS without any frame as a dead link. */

/* ---- commands --------------------------------------------------------------------------------- */

#define OTA_CMD_NOTIFY        0xA0
#define OTA_CMD_BLOCK         0xA1
#define OTA_CMD_RESULT        0xA2
#define OTA_CMD_IDENT         0xA3

/* 0xA0 offer, ZTU -> app. Written by: ZTU p1 (9 bytes), ZTU p3 (11). Read by: 2.1.x app (bytes 0-8,
 * len >= 9), v2 app (build rule from 11 bytes, byte rule below). Answer: 1 byte, 0 = accepted. */
#define OTA_NOTIFY_LEN_V1     9u
#define OTA_NOTIFY_LEN        11u
#define OTA_NOTIFY_AT_VERSION 0u      /* version byte of the carried image */
#define OTA_NOTIFY_AT_SIZE    1u      /* size, BE */
#define OTA_NOTIFY_AT_CRC     5u      /* CRC, BE: the app writes it to the mailbox (expect_crc) */
#define OTA_NOTIFY_AT_BUILD   9u      /* build of the carried image, BE (ZTU p3) */
#define OTA_NOTIFY_ACCEPT     0u

/* 0xA1 block request, bootloader -> ZTU. v1 boot: 6 bytes; v2 boot: 16. ZTU p1/p3 serve from bytes
 * 0-5 only, for ever (contract item 1); ZTU p3 logs bytes 6-15. A 6-byte request = a v1 boot. */
#define OTA_REQ_LEN_V1        6u
#define OTA_REQ_LEN           16u
#define OTA_REQ_AT_OFFSET     0u      /* image offset wanted, BE */
#define OTA_REQ_AT_MAX_LEN    4u      /* at most this many bytes, BE (the v2 boot asks <= OTA_BLOCK_MAX) */
#define OTA_REQ_AT_BOOT       6u      /* bootloader version (BOOT_VERSION) */
#define OTA_REQ_AT_MODE       7u      /* OTA_MODE_* */
#define OTA_REQ_AT_INSTALLED  8u      /* the installed header's bytes 8..15 as in flash: CRC (LE), version,
                                         flags, build (LE); meaningless in OTA_MODE_NO_APP */

/* Request mode (byte 7): why the v2 bootloader asks. A mode also covers the update it started; after a
 * give-up without a valid app the mode becomes OTA_MODE_NO_APP. */
#define OTA_MODE_ASKED        1u      /* the app accepted an offer (mailbox magic) */
#define OTA_MODE_NO_APP       2u      /* no valid app (or the give-up dropped it) */
#define OTA_MODE_PROBE        3u      /* power-on probe */
#define OTA_MODE_RESCUE       4u      /* 3 unconfirmed boots (try counter) */
#define OTA_MODE_BUTTON       5u      /* module button at power-on */
#define OTA_MODE_LINK         6u      /* probe asked by the app's link watchdog (BOOT_PROBE_REQ) */

/* 0xA1 block answer, ZTU -> bootloader: offset size crc version, then the image bytes. Stateless on the
 * ZTU side, for any offset; n = 0 past the end. Read by: v1 boot, v2 boot. */
#define OTA_BLOCK_HEAD        13u
#define OTA_BLOCK_AT_OFFSET   0u      /* BE, as asked */
#define OTA_BLOCK_AT_SIZE     4u      /* carried image size, BE */
#define OTA_BLOCK_AT_CRC      8u      /* carried image CRC, BE */
#define OTA_BLOCK_AT_VERSION  12u     /* carried version byte */
#define OTA_BLOCK_AT_DATA     13u
#define OTA_BLOCK_MAX         128u    /* the bootloaders ask for and take 1..128 bytes that fit the page */
#define OTA_ZTU_BLOCK_MAX     64u     /* what the ZTU serves today (fork BLOCK_MAX): may grow over the air */
#define OTA_HEADER_ANSWER_LEN (OTA_BLOCK_HEAD + (uint32_t)sizeof(app_header_t))   /* 29: the 38-byte frame */

/* 0xA2 result, bootloader -> ZTU. v1 boot: 2 bytes; v2 boot: 4. Read by: ZTU p1 (bytes 0-1), ZTU p3. */
#define OTA_RESULT_LEN_V1     2u
#define OTA_RESULT_LEN        4u
#define OTA_RESULT_AT_STATUS  0u
#define OTA_RESULT_AT_VERSION 1u      /* version byte of the image */
#define OTA_RESULT_AT_BUILD   2u      /* build of the written image, BE */
#define OTA_RESULT_OK         0       /* written and valid */
#define OTA_RESULT_BAD_IMAGE  1       /* CRC / header check failed after writing, or page 0 refused */
#define OTA_RESULT_FLASH      2       /* a page did not verify (twice) */

/* 0xA3 identity answer, app -> ZTU (the question has no data). 2.1.x app: 7 bytes; v2 app: 10.
 * Read by: ZTU p1 (len >= 7, fixed offsets 0-6), ZTU p3 (build from len >= 9, byte 9 when 10). */
#define OTA_IDENT_MAGIC       "ZDMS"
#define OTA_IDENT_KIND_APP    1
#define OTA_PROTOCOL          1       /* unchanged in v2: the ZTU offers nothing to another value */
#define OTA_IDENT_LEN_V1      7u
#define OTA_IDENT_LEN         10u
#define OTA_IDENT_AT_MAGIC    0u
#define OTA_IDENT_AT_KIND     4u
#define OTA_IDENT_AT_VERSION  5u
#define OTA_IDENT_AT_PROTOCOL 6u
#define OTA_IDENT_AT_BUILD    7u      /* FW_BUILD, BE */
#define OTA_IDENT_AT_BOOT     9u      /* boot_id.version, or OTA_BOOT_VERSION_V1 without a boot_id */

/* ---- v2 bootloader timing (frozen; docs/BOOTLOADER.md section 4) ----------------------------- */

#define PROBE_EVERY_MS        100u    /* header requests of a probe */
#define PROBE_WINDOW_MS       3000u   /* probe window (user decision 2026-11-18); ends at the first answer */
#define BUTTON_MS             100u    /* PA6 low on every pass for this long: pressed */
#define BUTTON_RELEASE_MS     5000u   /* ... and released within this: a rescue, else STUCK */
#define REQUEST_EVERY_MS      500u    /* request pace when nothing comes */
#define REJECT_PACE_MS        50u     /* a stray block is asked for again after this */
#define GIVE_UP_MS            10000u  /* after the last accepted block (or the taken header) */
#define RESCUE_WINDOW_MS      30000u  /* counter and button rescue windows (user decision 2026-11-18) */
#define FRAME_SILENCE_MS      50u     /* half a frame that stops coming is dropped */
#define RESULT_FLUSH_MS       20u     /* the result frame leaves before the reset */

/* ZTU contract item 3: the first header answer within this of power-up (half the probe window). */
#define OTA_ZTU_FIRST_ANSWER_MS 1500u

_Static_assert(PROBE_WINDOW_MS < 4096u, "the boot word holds the probe answer time in 12 bits");
_Static_assert(2u * OTA_ZTU_FIRST_ANSWER_MS <= PROBE_WINDOW_MS, "contract: half the probe window");

/* ---- RAM mailbox (the last 32 bytes of RAM) ---------------------------------------------------- */

/* Outside .data/.bss/stack in both linker scripts (RAM LENGTH = 3K - 32): survives a software,
 * watchdog or fault reset, random after a power-up. */
#define OTA_MAILBOX_RAM       0x20000BE0u
#ifndef OTA_MAILBOX_ADDR
#define OTA_MAILBOX_ADDR      OTA_MAILBOX_RAM
#endif
#define OTA_MAILBOX_MAGIC     0x55504454u   /* "UPDT" */

typedef struct {
    uint32_t magic;          /* 0x00 update request (OTA_MAILBOX_MAGIC). Written by: 2.1.x / v2 app after
                                accepting an offer. Read and cleared by: v1 boot, v2 boot (every boot) */
    uint32_t magic_inv;      /* 0x04 ~magic. Same */
    uint32_t restore_check;  /* 0x08 restore ^ OTA_RESTORE_MAGIC. App only (dimmer.c): no bootloader
                                ever touches it */
    uint32_t restore;        /* 0x0C the light state to come back to after a reset that kept the RAM.
                                App only (dimmer.c) */
    uint32_t expect_crc;     /* 0x10 the image the app accepted: the bootloader takes no other. Written
                                by: app with the magic. Read by: v1 boot, v2 boot when asked */
    uint32_t restored;       /* 0x14 OTA_RESTORED_MAGIC: this run already came back by restore, the next
                                reset takes the power-on behaviour. Read/written by: app. Written by:
                                v2 boot (OTA_RESTORED_MAGIC) only when it opens a counter rescue window */
    uint32_t boot;           /* 0x18 boot word (BOOT_*; was reserved[0]). Read and written by: v2 boot at
                                every boot. v2 app: reads at start (DP 137), clears the tries to confirm,
                                sets BOOT_PROBE_REQ before a link-watchdog reset. make flash, install.sh
                                and ota_swd_emu.py write 0 to it and boot_check before their resets.
                                The v1 boot ignores it */
    uint32_t boot_check;     /* 0x1C ~boot (was reserved[1]); rewritten with every change of boot */
} ota_mailbox_t;

#define OTA_RESTORE_MAGIC     0x52535452u   /* "RSTR" */
#define OTA_RESTORED_MAGIC    0x52535431u   /* "RST1" */

/* Boot word. Valid only when the tag matches and boot_check == ~boot (random RAM: p ~ 2^-40). Rebuilt
 * as BOOT_WORD_FRESH at a cold boot (power-up, brown-out, NRST, option reload); kept on a warm boot
 * (software or watchdog reset) when valid. DP 137 = its low 24 bits at the app's start.
 *   31..24  tag
 *   23..12  answer time of the last probe since the power-up, ms since its first request (1..4095),
 *           0 = no answer or no probe. A link probe (BOOT_INFO_LINK) replaces the value first.
 *   11..8   info since the power-up, set by the v2 boot
 *   7       BOOT_PROBE_REQ: app -> v2 boot, cleared by the v2 boot at every boot (honoured or not)
 *   6       BOOT_INFO_LINK: a probe asked by BOOT_PROBE_REQ ran since the power-up (v2 boot)
 *   5..4    0 (reserved)
 *   3..0    tries: unconfirmed boots of a counted app, 0..BOOT_TRIES_MAX. Always read through
 *           BOOT_TRIES_MASK: bits 7..4 are not part of it. */
#define BOOT_TAG              0xB2u
#define BOOT_TAG_SHIFT        24u
#define BOOT_WORD_FRESH       ((uint32_t)BOOT_TAG << BOOT_TAG_SHIFT)
#define BOOT_PROBE_MS_SHIFT   12u
#define BOOT_PROBE_MS_MASK    (0xFFFu << BOOT_PROBE_MS_SHIFT)
#define BOOT_INFO_RESCUE      (1u << 8)     /* the try counter opened a rescue window */
#define BOOT_INFO_BUTTON      (1u << 9)     /* the button opened one */
#define BOOT_INFO_WRITTEN     (1u << 10)    /* an image was written */
#define BOOT_INFO_STUCK       (1u << 11)    /* the button was still held BUTTON_RELEASE_MS after power-up */
#define BOOT_PROBE_REQ        (1u << 7)
#define BOOT_INFO_LINK        (1u << 6)
#define BOOT_RESERVED_BITS    (3u << 4)
#define BOOT_TRIES_MASK       0x0Fu
#define BOOT_TRIES_MAX        3u            /* a counted app with this many unconfirmed boots: rescue */
#define BOOT_WORD_VALID(w, check) (((w) >> BOOT_TAG_SHIFT) == BOOT_TAG && (check) == ~(w))

/* ---- app policy behind the boot word (v2 app; changes over the air, the bootloader never reads it) */

#define BOOT_CONFIRM_MS       30000u  /* uptime before a run may confirm (docs/BOOTLOADER.md section 7) */
#define BOOT_CONFIRM_QUERIES  2u      /* well-formed 0x01 queries without any 0xA3: a stock Tuya ZTU */
/* Link watchdog defaults (user decision 2026-11-18; section 9): fires only with a v2 boot that has
 * BOOT_FEAT_LINK, only while every channel is off, never in a bench build. */
#define LINK_WDG_SILENCE_MS   (30u * 60u * 1000u)        /* no well-formed frame from the ZTU for 30 min */
#define LINK_WDG_UPTIME_MS    (4u * 60u * 60u * 1000u)   /* and this run lasted 4 h: at most one per 4 h */
#define LINK_WDG_QUIET_MS     (60u * 1000u)              /* and a minute dark with no input: a switch
                                                            flipped back right after an off is never lost */

/* ---- layout checks: the offsets the frozen and the released code read ----------------------- */

_Static_assert(sizeof(app_header_t) == 16u, "app header: 16 bytes");
_Static_assert(offsetof(app_header_t, magic) == 0u && offsetof(app_header_t, size) == 4u &&
               offsetof(app_header_t, crc) == 8u && offsetof(app_header_t, version) == 12u &&
               offsetof(app_header_t, flags) == 13u && offsetof(app_header_t, build) == 14u,
               "app header offsets (v1 boot, ZTU p1)");
_Static_assert(sizeof(boot_id_t) == 16u && offsetof(boot_id_t, version) == 4u &&
               offsetof(boot_id_t, features) == 5u && offsetof(boot_id_t, app_max) == 8u &&
               offsetof(boot_id_t, mailbox) == 12u, "boot_id_t layout");
_Static_assert(sizeof(ota_mailbox_t) == 32u, "the mailbox is the last 32 bytes of RAM");
_Static_assert(offsetof(ota_mailbox_t, magic) == 0x00u && offsetof(ota_mailbox_t, magic_inv) == 0x04u &&
               offsetof(ota_mailbox_t, restore_check) == 0x08u && offsetof(ota_mailbox_t, restore) == 0x0Cu &&
               offsetof(ota_mailbox_t, expect_crc) == 0x10u && offsetof(ota_mailbox_t, restored) == 0x14u &&
               offsetof(ota_mailbox_t, boot) == 0x18u && offsetof(ota_mailbox_t, boot_check) == 0x1Cu,
               "mailbox offsets (v1 boot reads 0x00, 0x04, 0x10; tools clear 0x18, 0x1C)");
_Static_assert(OTA_MAILBOX_RAM + sizeof(ota_mailbox_t) == 0x20000C00u, "3 KB RAM");
_Static_assert((BOOT_WORD_FRESH & ~0xFF000000u) == 0u && (BOOT_PROBE_MS_MASK & 0xFF000FFFu) == 0u &&
               ((BOOT_INFO_RESCUE | BOOT_INFO_BUTTON | BOOT_INFO_WRITTEN | BOOT_INFO_STUCK) == 0xF00u) &&
               ((BOOT_PROBE_REQ | BOOT_INFO_LINK | BOOT_RESERVED_BITS | BOOT_TRIES_MASK) == 0xFFu) &&
               (BOOT_TRIES_MAX <= BOOT_TRIES_MASK), "boot word fields");
_Static_assert(APP_BASE + BOOT_APP_MAX <= FLASH_TOP - 0x800u && SETTINGS_BASE_LEGACY == 0x08004000u &&
               SETTINGS_BASE_HIGH == 0x08004800u && APP_BASE == BOOT_BASE + BOOT_SIZE, "flash layout");
_Static_assert(OTA_BLOCK_HEAD == OTA_BLOCK_AT_DATA && OTA_HEADER_ANSWER_LEN == 29u, "block answer");

#ifdef HOST_TEST
/* Host tests (tests/): the mailbox is a variable of the test. */
extern volatile ota_mailbox_t g_host_mailbox;
#undef OTA_MAILBOX_ADDR
#define OTA_MAILBOX_ADDR      ((uintptr_t)&g_host_mailbox)
#endif
#define OTA_MAILBOX           ((volatile ota_mailbox_t *)(uintptr_t)OTA_MAILBOX_ADDR)

#endif
