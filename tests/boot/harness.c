/* make test-boot: the v2 bootloader (boot/boot.c, unmodified) on the PC, against models of
 * everything around it (docs/BOOTLOADER.md section 11).
 *
 * - Flash (0x08000000, 20 KB), RAM (0x20000000, 3 KB + the mailbox) and the factory trim page
 *   (0x1FFF0000) are mapped at their chip addresses. Every boot is a fork(), so the
 *   bootloader's statics are fresh as after Reset_Handler; flash and RAM are shared, so they
 *   carry over from boot to boot as on the chip (RAM is randomised at a power-up).
 * - The peripherals are hooks (tests/boot/fake/py32f0xx.h). Time is a model clock: a SysTick
 *   read costs 20 us (one pass of a bootloader loop), a CRC byte 3.75 us, a page write 4.5 ms,
 *   a byte on the wire 87 us. SysTick's COUNTFLAG is a flag, as on the chip, so a loop pass that
 *   takes longer than 1 ms counts one ms.
 * - The IWDG counts (LSI 32768 Hz) and resets the chip when it runs out (RAM kept). 0xCCCC
 *   starts it from 0xFFF and does not reload a running one; only 0xAAAA reloads, with the RLR
 *   in effect; a PR / RLR write takes effect after the update time, a new PR at once on the
 *   count under way. A warm reset keeps it running with its counter, PR and RLR (the
 *   pessimistic reading of bench test B8), and after it ran out it starts again from RLR; a
 *   power-up stops it. The app that a boot jumps to runs it at PR 3 / RLR 1000 and has just
 *   reloaded it when it resets, unless a scenario says otherwise (app_watchdog()).
 * - USART1: the ZTU's bytes reach a one-byte data register one every 87 us; a byte that arrives
 *   while RXNE is still set is lost and sets ORE, as on the chip. TX takes 87 us per byte.
 * - The ZTU: a C port of the fork's answer_block() (stateless, any offset, BLOCK_MAX 64 or 128,
 *   failed results counted and paused after 3), with a boot time, answer latency, noise (drop,
 *   late duplicates, bit flips), a cut frame right before every answer, unsolicited frames, an
 *   image switch, a "serve below" limit, a phase-1 / phase-3 image gate or none, and fuzzed
 *   answers. Stock mode: the stock Tuya ZTU, which never answers 0xA1 / 0xA2 and sends its 0x01
 *   query every 1 s, optionally with 1 s phases of 9600-baud garbage between them. SWD mode:
 *   tools/ota_swd_emu.py, feeding g_inject.
 * - Checked at every boot: no rule broken in any register model, the gates never driven high,
 *   no flash write outside 0x08001000..0x080047FF (sector 0 and the top of the settings ring
 *   compared byte for byte), every request 16 bytes with the bootloader version, no request
 *   started while a frame was half received, no gap between watchdog refreshes over 250 ms, a
 *   valid boot word without the probe request, and at every jump a valid app and the state the
 *   app is handed (VTOR, SP, SysTick off, USART1 reset, LED off, gates low, pins, clock, IWDG).
 *
 * Built twice (Makefile test-boot): with a flash model (power cuts at every page, pages that
 * never verify or fail their first write only) and with the real src/flash_drv.c against the
 * register model of tests/host/flash_model.hpp (REAL_FLASH_DRV: stale flags, WRPERR at every or
 * only the first erase, BSY that never clears). Every scenario runs in a worker process of its
 * own (HARNESS_JOBS of them at a time, default one per CPU); the output comes in table order.
 * Usage: harness <image dir> [scenario...] */
#define _GNU_SOURCE
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "flash_drv.h"
#include "ota_proto.h"
#include "py32f0xx.h"

/* boot/boot.c */
void                      Reset_Handler(void);
extern const boot_id_t    k_boot_id;
extern volatile uint8_t   g_inject[];
extern volatile uint16_t  g_inject_len, g_inject_pos;

/* Startup symbols for Reset_Handler: no .data to copy (the RAM flash routine of the real
 * driver is plain host code), no .bss to clear (the fork gives fresh statics). */
uint32_t _estack, _sidata, _sdata, _sbss;
extern uint32_t _edata __attribute__((alias("_sdata")));
extern uint32_t _ebss __attribute__((alias("_sbss")));

#define US 1000ull
#define MS 1000000ull
#define S_ 1000000000ull

#define BYTE_NS        (87 * US)
#define TICK_NS        (20 * US)        /* a SysTick read: one loop pass */
#define CRC_PS_BYTE    3750000ull       /* 3.75 us per CRC byte, in ps */
#define PAGE_NS        (4500 * US)      /* erase ~3.5 ms + program ~1 ms */
#define REG_NS         250ull           /* an RCC or IWDG access (bounded-wait passes) */
#define LSI_START_NS   (100 * US)
#define IWDG_UPDATE_NS (160 * US)       /* PR / RLR update: ~5 LSI cycles */
#define GAP_LIMIT_NS   (250 * MS)

enum { OUT_RETURNED = 1, OUT_JUMP = 10, OUT_RESET = 11, OUT_CUT = 12, OUT_TIMEOUT = 13, OUT_BAD = 14, OUT_WDT = 15 };
enum { ZTU_OWN, ZTU_STOCK, ZTU_SWD };
enum { GATE_RAW, GATE_P1, GATE_P3 };    /* the ZTU's image check before it serves */
/* The cut frame before an answer (section 8, Receive: the rescan). BADSUM: a whole frame header
 * whose length takes in the answer's first bytes, so the answer starts inside a frame with a
 * bad checksum. LONG: a header cut after 6 bytes, so with the answer's 55 AA its length reads
 * 0x55AA, longer than any answer. Either way the answer is in the buffer when the cut frame
 * fails, and only a rescan of the buffered bytes finds it. */
enum { PREFIX_NONE, PREFIX_BADSUM, PREFIX_LONG };

#define IMG_MAX  0x7000u
#define LED_MAX  8192u

/* Shared by all boots of a scenario (MAP_SHARED): the configuration, the ZTU's state across MCU
 * boots, and the results of the last boot. */
typedef struct {
    /* configuration */
    uint32_t csr;                    /* RCC_CSR at the next boot */
    uint64_t limit_ns;               /* a boot that runs longer ends as OUT_TIMEOUT */
    int      ztu;                    /* ZTU_* */
    int      gate;                   /* GATE_* */
    int      serving;                /* own ZTU answers 0xA1 at all */
    uint32_t block_max;
    uint64_t ready_ns, ready_cfg_ns; /* the ZTU answers nothing before (this boot / after a cold) */
    uint32_t serve_below;            /* answers only the header and offsets below, 0: all */
    int      probe_lat_ms;           /* header answers this long after the request (+-1.5 ms) */
    double   drop_p, dup_p, flip_p;
    int      fuzz;
    uint32_t unsol_every_ms;         /* own ZTU: query-all + version frames every N ms */
    int      prefix;                 /* own ZTU: a cut frame right before every answer (PREFIX_*) */
    int      garbage;                /* stock ZTU: 1 s phases of 9600-baud garbage */
    long     switch_at_page, stop_at_page;
    long     cut_at_page;            /* power cut at this flash page write (counted over boots) */
    int      cut_after_program;      /* 0: after the erase, 1: after the program */
    long     bad_page;               /* this image page never verifies */
    long     hidden_bad;             /* n > 0: hidden pages from n - 1 on never verify */
    uint32_t hidden_writes;          /* page writes into the hidden flash */
    long     flaky_page;             /* this image page fails its first write only */
    int      button;                 /* module button held from power-up */
    uint64_t button_release_ns;      /* released at (0: never) */
    int      lsi_dead;
    int      hsi_dead;                /* HSIRDY never set after the frequency change */
    uint32_t drv_sr;                 /* real driver: SR flags at reset */
    int      drv_bsy_stuck;
    int      drv_wrperr;             /* real driver: 1 every erase raises WRPERR, 2 the first erase of a boot */
    long     corrupt_off;            /* the first answer that holds this image offset has it wrong */
    int32_t  size_skew;              /* the ZTU's block header tells a size this much off the image's */
    uint32_t offset_skew;            /* the ZTU's block answers tell this offset more than asked */
    uint64_t reset_at_ns;            /* a reset that keeps RAM (watchdog, NRST glitch) at this time */
    /* the IWDG at the next reset (kept over a warm reset, stopped by a power-up) */
    int      wd_run;
    uint32_t wd_pr, wd_rlr;
    uint64_t wd_left_ns;             /* until it runs out, from the reset */
    int      ignore_max_len;         /* the ZTU sends BLOCK_MAX bytes whatever was asked (contract break) */
    int      switch_on_bad;          /* the ZTU switches to img2 at the first BAD_IMAGE result */
    unsigned seed;
    /* the ZTU across MCU boots */
    uint8_t  img[IMG_MAX], img2[IMG_MAX];
    uint32_t img_size, img2_size;
    int      failures;               /* failed results in a row: 3 pause the serving */
    uint32_t results_ok, results_bad, results_flash;
    long     pages_total;            /* flash page writes since the scenario start */
    /* results of the last boot */
    int      outcome;
    uint64_t t_end;
    char     violation[200];
    uint32_t requests, header_req, off0_req, by_mode[8];
    uint64_t first_req_ns, max_req_gap_ns, last_req_ns;
    uint32_t mid_frame;
    uint32_t answers;                /* 0xA1 answers the ZTU sent */
    uint8_t  req0[OTA_REQ_LEN + 9];  /* the first request frame */
    uint32_t res[3];
    uint16_t res_build;
    uint8_t  res_version;
    uint32_t rx_lost, pages, max_write_end, flash_calls;
    uint64_t iwdg_max_gap;
    int      iwdg_running;
    int      wd_fired;               /* the boot ended because the IWDG ran out */
    uint64_t wd_reload_ns;           /* the first reload with PR 3 / RLR 1000 in effect */
    int      jump_valid;
    uint32_t led_n;
    struct { uint64_t t; uint8_t on; } led[LED_MAX];
    uint64_t worst_gap;              /* over the whole run */
    uint32_t drv_erases, drv_programs, drv_unlocks, drv_locked_idle;
    uint32_t trace_first_gpio, trace_ok;   /* register trace checks (regs scenario) */
    uint32_t sp_at_jump;
    uint32_t swd_frames, wire_bytes;  /* frames fed through g_inject; bytes the MCU got on the UART */
    uint64_t lsi_wait_ns;
} shared_t;

static shared_t *S;
static uint8_t  *FLASH_MEM, *RAM_MEM, *TRIM_MEM;
static const char *g_dir;
static const char *g_scen = "";
static int       g_fail;

#define MB ((volatile ota_mailbox_t *)OTA_MAILBOX)

/* ---- model clock and the end of a boot -------------------------------------------------------- */

static uint64_t t_ns;               /* since this boot's reset */
static uint64_t crc_ps;             /* CRC time not yet charged, ps */
static void     finish(int o) __attribute__((noreturn));
static int      wd_counting;        /* the IWDG counts (wd_check(), with the model below) */
static void     wd_check(void);

static void advance(uint64_t ns) {
    t_ns += ns;
    if (t_ns > S->limit_ns) {
        finish(OUT_TIMEOUT);
    }
    if (S->reset_at_ns && t_ns >= S->reset_at_ns) {
        finish(OUT_WDT);
    }
    if (wd_counting) {
        wd_check();
    }
}

static void violation(const char *fmt, ...) {
    if (!S->violation[0]) {
        va_list ap;
        va_start(ap, fmt);
        int n = snprintf(S->violation, sizeof S->violation, "t %.3f s: ", t_ns / 1e9);
        vsnprintf(S->violation + n, sizeof S->violation - (size_t)n, fmt, ap);
        va_end(ap);
    }
}

static unsigned rnd(void) {
    S->seed = S->seed * 1103515245u + 12345u;
    return (S->seed >> 8) & 0xFFFFFF;
}

static double frand(void) {
    return rnd() / (double)0x1000000;
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t rd32be(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t zcrc(uint32_t c, const uint8_t *p, uint32_t n) {
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

/* The harness's own image check (the rule of docs/BOOTLOADER.md section 4, "Jump checks"), on
 * the image's bytes in a row. */
static int image_ok(const uint8_t *img, uint32_t max) {
    uint32_t size = rd32le(img + 0xC4), sp = rd32le(img), rv = rd32le(img + 4);
    if (rd32le(img + 0xC0) != APP_MAGIC || size < 0xD0 || size > max || (sp >> 20) != 0x200 ||
        rv < APP_BASE || rv >= APP_BASE + (size < BOOT_APP_MAX ? size : BOOT_APP_MAX)) {
        return 0;
    }
    uint8_t z[4] = { 0 };
    uint32_t c = zcrc(0, img, 0xC8);
    c = zcrc(c, z, 4);
    c = zcrc(c, img + 0xCC, size - 0xCC);
    return c == rd32le(img + 0xC8);
}

/* The installed image's bytes in a row (v3: from BOOT_APP_MAX on at HIDDEN_BASE), and its check. */
static void installed_bytes(uint8_t *d) {
    memcpy(d, FLASH_MEM + 0x1000, BOOT_APP_MAX);
    memcpy(d + BOOT_APP_MAX, FLASH_MEM + (HIDDEN_BASE - BOOT_BASE), HIDDEN_SIZE);
}

static int installed_ok(uint32_t max) {
    static uint8_t d[BOOT_IMAGE_MAX];
    installed_bytes(d);
    return image_ok(d, max);
}

/* ---- the wire: ZTU -> MCU ------------------------------------------------------------------- */

typedef struct { uint64_t at; uint32_t frame; uint8_t b, last, track; } wire_t;
#define WIRE_N 65536u
static wire_t   wire[WIRE_N];
static uint32_t w_put, w_get;
static uint64_t ztu_tx_free;          /* the ZTU's TX line is busy until */
static uint32_t frame_id;

/* The receiver: one-byte DR. */
static int      rx_full, rx_ore;
static uint8_t  rx_dr;
static uint32_t rx_dr_frame;
static int      dr_access;            /* a DR access happened since the last sync */
/* The frame being delivered, for the half-duplex check. */
static uint32_t cur_frame;
static int      cur_read, cur_bad;

static void wire_put(uint64_t at, uint8_t b, uint32_t frame, int last, int track) {
    if (w_put - w_get >= WIRE_N) {
        violation("harness: wire buffer full");
        return;
    }
    wire_t *w = &wire[w_put++ % WIRE_N];
    w->at = at, w->b = b, w->frame = frame, w->last = (uint8_t)last, w->track = (uint8_t)track;
}

/* A frame from the ZTU, sent at `at` or when its line is free. `track`: a frame the bootloader
 * can buffer whole (the half-duplex check applies to it). */
static void ztu_send(const uint8_t *f, int n, uint64_t at, int track) {
    if (at < ztu_tx_free) {
        at = ztu_tx_free;
    }
    frame_id++;
    for (int i = 0; i < n; i++) {
        wire_put(at + BYTE_NS * (uint64_t)(i + 1), f[i], frame_id, i == n - 1, track && n <= 150);
    }
    ztu_tx_free = at + BYTE_NS * (uint64_t)n;
}

static void wire_deliver(void) {
    while (w_get != w_put && wire[w_get % WIRE_N].at <= t_ns) {
        wire_t *w = &wire[w_get++ % WIRE_N];
        if (w->frame != cur_frame) {
            cur_frame = w->frame;
            cur_read  = 0;
            cur_bad   = !w->track;
        }
        if (!rx_full) {
            rx_full     = 1;
            rx_dr       = w->b;
            rx_dr_frame = w->frame;
        } else {
            rx_ore = 1;               /* this byte is lost; DR keeps the old one */
            S->rx_lost++;
            if (w->frame == cur_frame) {
                cur_bad = 1;
            }
        }
    }
}

/* Is a well-formed frame half read by the MCU right now? */
static int frame_half_read(void) {
    if (cur_bad || cur_read == 0) {
        return 0;
    }
    /* bytes of the current frame still on the wire, or delivered and not read */
    int more_on_wire = w_get != w_put && wire[w_get % WIRE_N].frame == cur_frame;
    int in_dr        = rx_full && rx_dr_frame == cur_frame;
    return more_on_wire || in_dr;
}

/* ---- the wire: MCU -> ZTU, and the ZTU model ------------------------------------------------- */

static uint64_t tx_shift_end;         /* the byte in the shift register is out at */
static int      tx_dr = -1;           /* byte waiting in DR, -1 none */
static uint8_t  mf[512];              /* the MCU's frame as the ZTU parses it */
static int      mf_n;
static uint64_t next_unsol, next_stock;

static void ztu_on_frame(const uint8_t *f, int n, uint64_t at);

static void ztu_rx(uint8_t b, uint64_t at) {
    if (mf_n == 0 && b != 0x55) {
        violation("MCU sent a byte outside a frame: %02x", b);
        return;
    }
    if (mf_n < (int)sizeof mf) {
        mf[mf_n++] = b;
    }
    if (mf_n == 2 && b != 0xAA) {
        violation("MCU frame without 55 AA");
        mf_n = 0;
    }
    if (mf_n >= 8 && mf_n == 8 + (mf[6] << 8 | mf[7]) + 1) {
        uint8_t s = 0;
        for (int i = 0; i < mf_n - 1; i++) {
            s = (uint8_t)(s + mf[i]);
        }
        if (s != mf[mf_n - 1]) {
            violation("MCU frame with a bad checksum");
        } else {
            ztu_on_frame(mf, mf_n, at);
        }
        mf_n = 0;
    }
}

static void tx_start(uint8_t b, uint64_t at) {
    if (mf_n == 0) {                  /* a new frame from the MCU: was a frame half read? */
        if (frame_half_read()) {
            S->mid_frame++;
        }
    }
    tx_shift_end = at + BYTE_NS;
    ztu_rx(b, tx_shift_end);
}

static void tx_progress(void) {
    if (tx_dr >= 0 && t_ns >= tx_shift_end) {
        uint8_t b = (uint8_t)tx_dr;
        tx_dr     = -1;
        tx_start(b, tx_shift_end);
    }
}

/* The ZTU's image_info() before it serves: phase 1 (fork tuya_mcu_ota.c:78-102: magic, size,
 * 12 KB, CRC, no SIM / MEASURE), phase 3 (section 10, change 4: also DIAG, the vectors, the
 * build range), or none at all (a ZTU the bootloader must not trust). */
static int image_gate(void) {
    const uint8_t *im = S->img;
    uint32_t size = rd32le(im + 0xC4);
    uint8_t  flags = im[0xCD];
    uint16_t build = (uint16_t)(im[0xCE] | im[0xCF] << 8);
    if (S->gate == GATE_RAW) {
        return 1;
    }
    uint8_t  z[4] = { 0 };
    uint32_t c    = size >= 0xD0 && size <= IMG_MAX ? zcrc(zcrc(zcrc(0, im, 0xC8), z, 4), im + 0xCC, size - 0xCC) : 0;
    if (rd32le(im + 0xC0) != APP_MAGIC || size != S->img_size || size > 0x3000 || c != rd32le(im + 0xC8) ||
        (flags & (APP_FLAG_SIM | APP_FLAG_MEASURE))) {
        return 0;
    }
    if (S->gate == GATE_P3) {
        uint32_t rv = rd32le(im + 4);
        if ((flags & APP_FLAG_DIAG) || rd32le(im) != 0x20000BE0u || !(rv & 1) || rv < APP_BASE ||
            rv >= APP_BASE + size || (build >= 1 && build <= 255) || build >= 0xFFFE) {
            return 0;
        }
    }
    return 1;
}

/* SWD mode: tools/ota_swd_emu.py puts the ZTU's answers into g_inject, one frame at a time. */
static uint8_t  swd_q[64][160];
static int      swd_qn[64], swd_head, swd_tail;
static uint32_t swd_frames;           /* frames placed into g_inject */

static void swd_put(const uint8_t *f, int n) {
    if (swd_head - swd_tail < 64 && n <= 150) {
        memcpy(swd_q[swd_head % 64], f, (size_t)n);
        swd_qn[swd_head % 64] = n;
        swd_head++;
    }
}

static void swd_feed(void) {
    if (S->ztu != ZTU_SWD || swd_head == swd_tail || g_inject_pos < g_inject_len) {
        return;
    }
    int n = swd_qn[swd_tail % 64];
    g_inject_len = 0;
    for (int i = 0; i < n; i++) {
        g_inject[i] = swd_q[swd_tail % 64][i];
    }
    g_inject_pos = 0;
    g_inject_len = (uint16_t)n;
    swd_tail++;
    swd_frames++;
}

static void ztu_answer_block(const uint8_t *f, int n, uint64_t at) {
    const uint8_t *d = f + 8;
    uint32_t off = rd32be(d);
    uint16_t want = (uint16_t)(d[4] << 8 | d[5]);
    if (!S->serving || S->failures >= 3 || at < S->ready_ns || !image_gate()) {
        return;
    }
    if (S->serve_below && off >= S->serve_below && off != APP_HEADER_OFFSET) {
        return;
    }
    const uint8_t *im = S->img;
    uint32_t size = rd32le(im + 0xC4), crc = rd32le(im + 0xC8);
    uint16_t m = 0;
    if (off < size) {
        m = want < S->block_max ? want : (uint16_t)S->block_max;
        if (S->ignore_max_len && off != APP_HEADER_OFFSET) {
            m = (uint16_t)S->block_max;          /* also past the image end */
        } else if (m > size - off) {
            m = (uint16_t)(size - off);
        }
    }
    uint8_t  fr[8 + 13 + 160 + 1];
    uint16_t body = (uint16_t)(13 + m);
    fr[0] = 0x55, fr[1] = 0xAA, fr[2] = 2, fr[3] = f[3], fr[4] = f[4], fr[5] = OTA_CMD_BLOCK;
    wr32be(fr + 8, off + (off == APP_HEADER_OFFSET ? 0 : S->offset_skew));
    wr32be(fr + 12, size + (uint32_t)S->size_skew);
    wr32be(fr + 16, crc);
    fr[20] = im[0xCC];
    memcpy(fr + 21, im + off, m);
    if (S->corrupt_off >= 0 && (uint32_t)S->corrupt_off >= off && (uint32_t)S->corrupt_off < off + m) {
        fr[21 + S->corrupt_off - off] ^= 0x40;   /* wrong data under a good frame checksum, once */
        S->corrupt_off = -1;
    }
    S->answers++;
    if (S->fuzz) {                    /* mutated answers that still pass the frame checksum */
        unsigned k = rnd() % 10;
        if (k < 4) {
            for (unsigned j = 1 + rnd() % 4; j--;) {
                fr[8 + rnd() % body] = (uint8_t)rnd();
            }
        } else if (k < 6) {
            body = (uint16_t)(rnd() % (sizeof fr - 9));
            for (int i = 21; i < 8 + body; i++) {
                fr[i] = (uint8_t)rnd();
            }
        } else if (k < 7) {
            wr32be(fr + 12, rnd() % 0x5000);
            wr32be(fr + 16, rnd() & 1 ? crc : (uint32_t)rnd());
        } else if (k < 8) {
            wr32be(fr + 8, off ^ (1u << (rnd() % 16)));
        }
        if (rnd() % 8 == 0) {         /* and a random frame of another command */
            uint8_t g[48];
            uint16_t gb = (uint16_t)(rnd() % 30);
            g[0] = 0x55, g[1] = 0xAA, g[2] = 2, g[3] = 0, g[4] = 0;
            g[5] = rnd() & 1 ? OTA_CMD_BLOCK : (uint8_t)rnd();
            g[6] = 0, g[7] = (uint8_t)gb;
            for (int i = 8; i < 8 + gb; i++) {
                g[i] = (uint8_t)rnd();
            }
            uint8_t gs = 0;
            for (int i = 0; i < 8 + gb; i++) {
                gs = (uint8_t)(gs + g[i]);
            }
            g[8 + gb] = gs;
            ztu_send(g, 9 + gb, at + MS + rnd() % 2000 * US, 1);
        }
    }
    fr[6] = (uint8_t)(body >> 8), fr[7] = (uint8_t)body;
    uint8_t s = 0;
    for (int i = 0; i < 8 + body; i++) {
        s = (uint8_t)(s + fr[i]);
    }
    fr[8 + body] = s;
    int len = 8 + body + 1;
    if (S->drop_p > 0 && frand() < S->drop_p) {
        return;
    }
    int flipped = 0;
    if (S->flip_p > 0 && frand() < S->flip_p) {
        fr[8 + rnd() % (unsigned)(len - 8)] ^= (uint8_t)(1u << (rnd() % 8));
        flipped = 1;
    }
    uint64_t lat = (S->probe_lat_ms && off == APP_HEADER_OFFSET)
                       ? (uint64_t)S->probe_lat_ms * MS - 1500 * US + rnd() % 3000 * US
                       : 2000 * US + rnd() % 3000 * US;
    if (S->ztu == ZTU_SWD) {           /* through the debugger, not the UART */
        swd_put(fr, len);
        return;
    }
    if (S->prefix) {
        uint8_t p[8] = { 0x55, 0xAA, 0x02, 0x00, 0x00, OTA_CMD_BLOCK, 0x00, 0x00 };
        int     pn   = 6;
        if (S->prefix == PREFIX_BADSUM) {
            /* length l: l data bytes and the checksum byte come from the answer; one less if
             * the sum happens to match */
            int l = len - 1 < 29 ? len - 1 : 29;
            for (; l > 1; l--) {
                uint8_t ps = (uint8_t)(0x55 + 0xAA + 0x02 + OTA_CMD_BLOCK + l);
                for (int i = 0; i < l; i++) {
                    ps = (uint8_t)(ps + fr[i]);
                }
                if (ps != fr[l]) {
                    break;
                }
            }
            p[7] = (uint8_t)l;
            pn   = 8;
        }
        ztu_send(p, pn, at + lat, 0);  /* not a frame: the half-duplex check skips it */
    }
    ztu_send(fr, len, at + lat, !flipped);
    if (S->dup_p > 0 && frand() < S->dup_p) {
        ztu_send(fr, len, at + 400 * MS + rnd() % 300 * MS, !flipped);
    }
}

static void ztu_on_frame(const uint8_t *f, int n, uint64_t at) {
    uint8_t cmd = f[5];
    uint16_t len = (uint16_t)(f[6] << 8 | f[7]);
    const uint8_t *d = f + 8;
    if (cmd == OTA_CMD_BLOCK) {
        S->requests++;
        if (len != OTA_REQ_LEN || d[OTA_REQ_AT_BOOT] != BOOT_VERSION || d[OTA_REQ_AT_MODE] < 1 ||
            d[OTA_REQ_AT_MODE] > 6) {
            violation("request of %u bytes, boot version %u, mode %u", len, d[6], d[7]);
            return;
        }
        uint32_t off = rd32be(d);
        uint16_t want = (uint16_t)(d[4] << 8 | d[5]);
        if (want == 0 || want > OTA_BLOCK_MAX) {
            violation("request for %u bytes", want);
        }
        if (off == APP_HEADER_OFFSET && want == 16) {
            S->header_req++;
        }
        if (off == 0) {
            S->off0_req++;
        }
        S->by_mode[d[OTA_REQ_AT_MODE]]++;
        if (S->requests == 1) {
            memcpy(S->req0, f, sizeof S->req0);
            S->first_req_ns = at;
        } else if (at - S->last_req_ns > S->max_req_gap_ns) {
            S->max_req_gap_ns = at - S->last_req_ns;
        }
        S->last_req_ns = at;
        if (S->ztu == ZTU_OWN || S->ztu == ZTU_SWD) {
            ztu_answer_block(f, n, at);
        }
    } else if (cmd == OTA_CMD_RESULT) {
        if (len != OTA_RESULT_LEN) {
            violation("result of %u bytes", len);
            return;
        }
        S->res_version = d[1];
        S->res_build   = (uint16_t)(d[2] << 8 | d[3]);
        if (d[0] <= 2) {
            S->res[d[0]]++;
        }
        if (S->ztu == ZTU_STOCK) {
            return;                   /* the stock ZTU drops 0xA2: (cmd - 1) > 0x39 */
        }
        if (d[0] == OTA_RESULT_OK) {
            S->results_ok++;
            S->failures = 0;
        } else {
            S->failures++;
            if (d[0] == OTA_RESULT_FLASH) {
                S->results_flash++;
            } else {
                S->results_bad++;
                if (S->switch_on_bad) {
                    S->switch_on_bad = 0;
                    memcpy(S->img, S->img2, IMG_MAX);
                    S->img_size = S->img2_size;
                }
            }
        }
    } else {
        violation("MCU sent command %02x", cmd);
    }
}

/* Frames the ZTU sends by itself. */
static void ztu_own_frames(void) {
    if (S->ztu == ZTU_OWN && S->unsol_every_ms && t_ns >= next_unsol && t_ns >= S->ready_ns) {
        static const uint8_t qa[]  = { 0x55, 0xAA, 0x02, 0x00, 0x05, 0x28, 0x00, 0x00, 0x2E };
        static const uint8_t ver[] = { 0x55, 0xAA, 0x02, 0x00, 0x06, 0x0B, 0x00, 0x00, 0x12 };
        next_unsol = t_ns + S->unsol_every_ms * MS;
        ztu_send(qa, sizeof qa, t_ns, 1);
        ztu_send(ver, sizeof ver, t_ns, 1);
    }
    if (S->ztu == ZTU_STOCK) {
        /* generate a few ms ahead, in time order */
        while (next_stock <= t_ns + 2 * MS) {
            uint64_t from_ready = next_stock > S->ready_ns ? next_stock - S->ready_ns : 0;
            int      garbage    = S->garbage && (from_ready / S_) % 2 == 1;
            if (next_stock < S->ready_ns) {
                next_stock = S->ready_ns;
            } else if (garbage) {     /* 9600 baud seen at 115200: a random byte per ~1.04 ms */
                uint8_t b = (uint8_t)rnd();
                ztu_send(&b, 1, next_stock, 0);
                next_stock += 1042 * US;
            } else {                  /* its product query: 55 AA 02 00 nn 01 00 00 cs */
                static uint8_t seq;
                uint8_t q[9] = { 0x55, 0xAA, 0x02, 0x00, ++seq, 0x01, 0x00, 0x00, 0 };
                for (int i = 0; i < 8; i++) {
                    q[8] = (uint8_t)(q[8] + q[i]);
                }
                ztu_send(q, 9, next_stock, 1);
                next_stock += S_;
            }
        }
    }
}

/* ---- the peripherals ------------------------------------------------------------------------- */

static RCC_TypeDef   rcc;
static GPIO_TypeDef  gpioa;
USART_TypeDef        host_usart;
static SysTick_Type  systick;
static IWDG_TypeDef  iwdg;
static SCB_Type      scb;

static uint32_t rcc_flags, rcc_csr_shown, apbrstr2_last;
static uint64_t lsi_on_at;
static int      lsion, usart_reset_set, usart_reset_pulsed;
static uint32_t odr, gpio_writes;
static uint32_t moder_last, pupdr_last;
static uint32_t systick_ctrl, systick_shown;
static uint64_t ms_seen;
static uint64_t iwdg_last_refresh, iwdg_update_until;
static int      iwdg_started, iwdg_unlocked, iwdg_seq;
static uint32_t iwdg_pr_last, iwdg_rlr_last;
static int      wd_pending;                        /* a PR / RLR write not in effect yet */
static uint32_t wd_pr, wd_rlr, wd_pr_new, wd_rlr_new;
static uint64_t wd_fire_ns;                        /* when the count runs out */
static int      irq_on = 1;

/* The LED (PA5, active low) is lit only while PA5 is an output driven low: at reset it is an
 * analog input. */
static void led_note(void) {
    uint8_t on = ((gpioa.MODER >> 10) & 3u) == 1u && !(odr & (1u << 5));
    if (S->led_n == 0 || S->led[S->led_n - 1].on != on) {
        if (S->led_n < LED_MAX) {
            S->led[S->led_n].t  = t_ns;
            S->led[S->led_n].on = on;
            S->led_n++;
        }
    }
}

static void iwdg_refresh(void) {
    if (lsion && !S->lsi_dead) {
        uint64_t gap = t_ns - iwdg_last_refresh;
        if (gap > S->iwdg_max_gap) {
            S->iwdg_max_gap = gap;
        }
    }
    iwdg_last_refresh = t_ns;
}

/* What the code did to a peripheral since its previous access, caught at the next access of the
 * same peripheral (the UART model also catches up with the wire then): a key or a BRR write is
 * seen at most one loop pass late, which shifts the watchdog gaps but never shortens one. */
static void sync_rcc(void) {
    if (rcc.CSR != rcc_csr_shown) {
        uint32_t v = rcc.CSR;
        if (v & RCC_CSR_RMVF) {
            violation("the bootloader wrote RMVF");
        }
        if ((v & RCC_CSR_LSION) && !lsion) {
            lsion     = 1;
            lsi_on_at = t_ns;
        }
        rcc_flags     = 0;            /* pessimistic: the flags must have been copied before */
        rcc_csr_shown = v;
    }
    if (rcc.APBRSTR2 != apbrstr2_last) {
        if (rcc.APBRSTR2 & RCC_APBRSTR2_USART1RST) {
            usart_reset_set = 1;
        } else if (usart_reset_set) {
            usart_reset_pulsed = 1;
        }
        apbrstr2_last = rcc.APBRSTR2;
    }
}

static void sync_gpio(void) {
    if (gpioa.BRR || gpioa.BSRR) {
        if (!(rcc.IOPENR & RCC_IOPENR_GPIOAEN)) {
            violation("GPIOA written with its clock off");
        }
        if (gpio_writes++ == 0) {
            S->trace_first_gpio = gpioa.BRR ? gpioa.BRR : 0x80000000u | gpioa.BSRR;
        }
        if (gpioa.BSRR & 0x3u) {
            violation("a gate was driven high (BSRR %08x)", gpioa.BSRR);
        }
        odr &= ~gpioa.BRR;
        odr |= gpioa.BSRR & 0xFFFFu;
        odr &= ~(gpioa.BSRR >> 16);
        gpioa.BRR = gpioa.BSRR = 0;
        led_note();
    }
    if (gpioa.MODER != moder_last || gpioa.PUPDR != pupdr_last) {
        if (gpio_writes == 0) {
            S->trace_first_gpio = 0x40000000u;   /* MODER or PUPDR before the gates were low */
            gpio_writes = 1;
        }
        moder_last = gpioa.MODER;
        pupdr_last = gpioa.PUPDR;
        led_note();
    }
}

static void sync_systick(void) {
    if (systick.CTRL != systick_shown) {   /* a write shows as a value other than the one shown */
        systick_ctrl = systick.CTRL & ~SysTick_CTRL_COUNTFLAG_Msk;
    }
}

/* One count of the IWDG: LSI / (4 << PR); PR 6 and 7 both divide by 256. */
static uint64_t wd_tick_ns(uint32_t pr) {
    return (4ull << (pr > 6 ? 6 : pr)) * S_ / 32768u;
}

/* A PR / RLR write takes effect once the update time has passed (never with a dead LSI): a new
 * PR changes the pace of the count under way, a new RLR only the next reload. */
static void wd_settle(void) {
    if (!wd_pending || S->lsi_dead || t_ns < iwdg_update_until) {
        return;
    }
    if (wd_counting && wd_pr_new != wd_pr) {
        uint64_t at = iwdg_update_until, left = wd_fire_ns > at ? wd_fire_ns - at : 0;
        wd_fire_ns = at + left / wd_tick_ns(wd_pr) * wd_tick_ns(wd_pr_new);
    }
    wd_pr      = wd_pr_new;
    wd_rlr     = wd_rlr_new;
    wd_pending = 0;
}

static void sync_iwdg(void) {
    if (iwdg.KR) {                    /* a key shows in KR, which reads 0 */
        uint32_t k = iwdg.KR;
        iwdg.KR = 0;
        wd_settle();
        if (k == 0xCCCC) {
            iwdg_started = 1;
            if (!wd_counting && !S->lsi_dead) {   /* started from 0xFFF; a running one goes on */
                wd_counting = 1;
                wd_fire_ns  = t_ns + 0xFFFu * wd_tick_ns(wd_pr);
                iwdg_refresh();
            }
            if (iwdg_seq == 0) {
                iwdg_seq = 1;
            }
        } else if (k == 0xAAAA) {
            if (wd_counting) {
                wd_fire_ns = t_ns + wd_rlr * wd_tick_ns(wd_pr);
            }
            iwdg_refresh();
            /* the trace: reloaded with PR 3 / RLR 1000 in effect, after the start and the unlock */
            if (iwdg_seq == 2 && iwdg.PR == 3u && iwdg.RLR == 1000u && (!wd_pending || S->lsi_dead)) {
                iwdg_seq        = 3;
                S->wd_reload_ns = t_ns;
            }
        } else if (k == 0x5555) {
            iwdg_unlocked = 1;
            if (iwdg_seq == 1) {
                iwdg_seq = 2;
            }
        } else {
            violation("IWDG key %x", k);
        }
    }
    if (iwdg.PR != iwdg_pr_last || iwdg.RLR != iwdg_rlr_last) {
        if (!iwdg_unlocked) {
            violation("IWDG PR/RLR written while locked");
        }
        wd_settle();
        iwdg_pr_last      = iwdg.PR;
        iwdg_rlr_last     = iwdg.RLR;
        iwdg_update_until = t_ns + IWDG_UPDATE_NS;
        wd_pr_new         = iwdg.PR;
        wd_rlr_new        = iwdg.RLR;
        wd_pending        = 1;
    }
}

/* From advance(), while the IWDG counts: the keys written so far, then the reset when it runs
 * out. */
static void wd_check(void) {
    sync_iwdg();
    wd_settle();
    if (wd_counting && t_ns >= wd_fire_ns) {
        S->wd_fired = 1;
        finish(OUT_WDT);
    }
}

static void sync_usart(void) {
    if (dr_access) {                  /* a DR access, read or write */
        dr_access = 0;
        uint32_t v = host_usart.DR_[0];
        if (v & 0x100u) {             /* the marker is still there: a read */
            if (rx_full && rx_dr_frame == cur_frame) {
                cur_read++;
            }
            rx_full = 0;
            rx_ore  = 0;
        } else if (tx_dr >= 0) {
            violation("DR written while TXE was clear");
        } else if (t_ns >= tx_shift_end) {
            tx_start((uint8_t)v, t_ns);
        } else {
            tx_dr = (uint8_t)v;
        }
    }
    tx_progress();
    ztu_own_frames();
    wire_deliver();
}

static void model_sync(void) {
    sync_rcc();
    sync_gpio();
    sync_systick();
    sync_iwdg();
    sync_usart();
}

RCC_TypeDef *host_rcc(void) {
    sync_rcc();
    advance(REG_NS);
    rcc.CR = S->hsi_dead ? 0 : RCC_CR_HSIRDY;
    int rdy = lsion && !S->lsi_dead && t_ns >= lsi_on_at + LSI_START_NS;
    rcc_csr_shown = rcc_flags | (lsion ? RCC_CSR_LSION : 0) | (rdy ? RCC_CSR_LSIRDY : 0);
    rcc.CSR = rcc_csr_shown;
    return &rcc;
}

GPIO_TypeDef *host_gpioa(void) {
    sync_gpio();
    int pressed = S->button && (!S->button_release_ns || t_ns < S->button_release_ns);
    gpioa.IDR = 0xFFFFu & ~(pressed ? (1u << 6) : 0);
    return &gpioa;
}

/* USART1->SR and IWDG->SR (only the IWDG update wait reads the latter, never while the UART
 * sends): both are set before the read. A read while the UART sends is a pass of a TXE / TC
 * wait. */
uint32_t host_sr(void) {
    sync_usart();
    sync_iwdg();
    int txe = tx_dr < 0, tc = txe && t_ns >= tx_shift_end;
    if (!tc) {
        advance(1 * US);
        sync_usart();
        txe = tx_dr < 0, tc = txe && t_ns >= tx_shift_end;
    } else {
        advance(REG_NS);
    }
    host_usart.SR_[0] = (rx_full ? USART_SR_RXNE : 0) | (rx_ore ? USART_SR_ORE : 0) | (txe ? USART_SR_TXE : 0) |
                        (tc ? USART_SR_TC : 0);
    iwdg.SR_[0] = (t_ns < iwdg_update_until || (S->lsi_dead && iwdg_update_until)) ? 3u : 0u;
    return 0;
}

uint32_t host_usart_dr(void) {
    sync_usart();
    dr_access         = 1;
    host_usart.DR_[0] = 0x100u | rx_dr;    /* the marker: still there afterwards = a read */
    return 0;
}

SysTick_Type *host_systick(void) {
    sync_systick();
    swd_feed();
    advance(TICK_NS);
    uint32_t flag = 0;
    if ((systick_ctrl & SysTick_CTRL_ENABLE_Msk) && t_ns / MS != ms_seen) {
        ms_seen = t_ns / MS;
        flag    = SysTick_CTRL_COUNTFLAG_Msk;
    }
    systick_shown = systick_ctrl | flag;
    systick.CTRL  = systick_shown;
    return &systick;
}

IWDG_TypeDef *host_iwdg(void) {
    sync_iwdg();
    advance(REG_NS);
    return &iwdg;
}

SCB_Type *host_scb(void) {
    return &scb;
}

void host_irq(int enabled) {
    irq_on = enabled;
}

static void finish(int o) {
    if (o != OUT_TIMEOUT && o != OUT_CUT) {
        model_sync();
    }
    if (lsion && !S->lsi_dead && t_ns - iwdg_last_refresh > S->iwdg_max_gap) {
        S->iwdg_max_gap = t_ns - iwdg_last_refresh;
    }
    S->iwdg_running = iwdg_started && lsion && !S->lsi_dead;
    /* the IWDG for the next reset: what was written is in effect by then; after it ran out it
     * starts again from RLR (boot_once() and cold() set what an app or a power-up leaves) */
    if (wd_pending && !S->lsi_dead) {
        wd_pr = wd_pr_new, wd_rlr = wd_rlr_new;
    }
    S->wd_run     = wd_counting;
    S->wd_pr      = wd_pr;
    S->wd_rlr     = wd_rlr;
    S->wd_left_ns = S->wd_fired ? wd_rlr * wd_tick_ns(wd_pr) : wd_fire_ns > t_ns ? wd_fire_ns - t_ns : 0;
    S->swd_frames   = swd_frames;
    S->wire_bytes   = w_get;
    S->outcome      = o;
    S->t_end        = t_ns;
#ifdef REAL_FLASH_DRV
    void drv_model_stats(uint32_t *, uint32_t *, uint32_t *, uint32_t *);
    drv_model_stats(&S->drv_erases, &S->drv_programs, &S->drv_unlocks, &S->drv_locked_idle);
#endif
    _exit(o);
}

/* The jump into the app (__set_MSP in jump_app()). */
void host_set_msp(uint32_t sp) {
    model_sync();
    S->sp_at_jump = sp;
    S->jump_valid = installed_ok(BOOT_IMAGE_MAX);
    uint32_t ok = 0;
    ok |= (scb.VTOR == APP_BASE) << 0;
    ok |= (systick_ctrl == 0) << 1;
    ok |= usart_reset_pulsed << 2;
    ok |= (!irq_on) << 3;
    ok |= ((odr & (1u << 5)) != 0) << 4;            /* LED off */
    ok |= ((odr & 3u) == 0) << 5;                   /* gates low */
    ok |= (sp == rd32le(FLASH_MEM + 0x1000)) << 6;
    ok |= ((gpioa.MODER & 0x3C0Fu) == 0x0405u) << 7;   /* PA0 PA1 PA5 out, PA6 in */
    ok |= ((gpioa.PUPDR & (3u << 12)) == (1u << 12)) << 8;
    ok |= ((gpioa.MODER & 0xF0u) == 0xA0u && (gpioa.AFR[0] & 0xFF00u) == 0x1100u) << 9;   /* PA2 PA3 AF1 */
    ok |= (host_usart.BRR == 208u && host_usart.CR1 == (USART_CR1_UE | USART_CR1_TE | USART_CR1_RE)) << 10;
    ok |= ((rcc.ICSCR >> 13 & 7u) == 4u && (rcc.ICSCR & 0x1FFFu) == (rd32le(TRIM_MEM + 0xF10) & 0x1FFFu)) << 11;
    ok |= (systick.LOAD == 23999u) << 12;
    ok |= (iwdg_seq == 3 && iwdg.PR == 3u && iwdg.RLR == 1000u) << 13;   /* start, unlock, PR / RLR, reload */
    S->trace_ok = ok;
    finish(OUT_JUMP);
}

void host_system_reset(void) {
    finish(OUT_RESET);
}

/* ---- flash ---------------------------------------------------------------------------------- */

static __attribute__((unused)) void cut_now(void) {
    finish(OUT_CUT);
}

#ifdef REAL_FLASH_DRV
uint32_t drv_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);
void     drv_model_init(void (*advance_ns)(uint32_t), uint32_t sr, int bsy_stuck, int wrperr);

uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    crc_ps += (uint64_t)n * CRC_PS_BYTE;
    advance(crc_ps / 1000);
    crc_ps %= 1000;
    return drv_crc32_update(crc, p, n);
}

static void drv_advance(uint32_t ns) {
    advance(ns);
}
#else
uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    crc_ps += (uint64_t)n * CRC_PS_BYTE;
    advance(crc_ps / 1000);
    crc_ps %= 1000;
    return zcrc(crc, p, n);
}

/* As src/flash_drv.c, with the range of boot.ld: refuses outside the app area and the hidden
 * flash (never the ring between them); a page that already holds src is not erased; else
 * ~4.5 ms. Power cuts after the erase or the program. hidden_bad: no hidden page programs. */
uint8_t flash_write_page(uint32_t addr, const uint32_t *src) {
    uint8_t *p = (uint8_t *)(uintptr_t)addr;
    S->flash_calls++;
    if ((addr & 127u) || addr < APP_BASE || addr + 128u > HIDDEN_BASE + HIDDEN_SIZE ||
        (addr + 128u > APP_BASE + BOOT_APP_MAX && addr < HIDDEN_BASE)) {
        violation("page write at %08x", addr);
        finish(OUT_BAD);
    }
    if (addr >= HIDDEN_BASE) {
        S->hidden_writes++;
    }
    if (!memcmp(p, src, 128)) {
        return 1;
    }
    advance(PAGE_NS);
    long k = S->pages_total++;
    S->pages++;
    if (addr + 128u > S->max_write_end) {
        S->max_write_end = addr + 128u;
    }
    if (S->switch_at_page >= 0 && k == S->switch_at_page) {
        memcpy(S->img, S->img2, IMG_MAX);
        S->img_size = S->img2_size;
        S->failures = 0;
    }
    if (S->stop_at_page >= 0 && k == S->stop_at_page) {
        S->serving = 0;
    }
    memset(p, 0xFF, 128);
    if (k == S->cut_at_page && !S->cut_after_program) {
        cut_now();
    }
    if ((S->bad_page >= 0 && addr == IMAGE_ADDR(128u * (uint32_t)S->bad_page)) ||
        (S->hidden_bad && addr >= HIDDEN_BASE + 128u * (uint32_t)(S->hidden_bad - 1))) {
        return 0;                     /* erased, never programs */
    }
    if (S->flaky_page >= 0 && addr == IMAGE_ADDR(128u * (uint32_t)S->flaky_page)) {
        S->flaky_page = -1;
        return 0;                     /* erased, not programmed this once */
    }
    memcpy(p, src, 128);
    if (k == S->cut_at_page && S->cut_after_program) {
        cut_now();
    }
    return 1;
}
#endif

/* ---- boots ------------------------------------------------------------------------------------ */

static uint8_t  boot_sector[0x1000], settings_top[0x800];

static void child_init(void) {
    t_ns = 0, crc_ps = 0;
    w_put = w_get = 0, ztu_tx_free = 0, frame_id = 0;
    rx_full = rx_ore = 0, dr_access = 0, cur_frame = 0, cur_read = cur_bad = 0;
    tx_shift_end = 0, tx_dr = -1, mf_n = 0;
    next_unsol = S->ready_ns + 2 * S_;
    next_stock = S->ready_ns + (S->ready_ns ? 0 : rnd() % 1000 * MS);
    memset(&rcc, 0, sizeof rcc), memset(&gpioa, 0, sizeof gpioa), memset(&host_usart, 0, sizeof host_usart);
    memset(&systick, 0, sizeof systick), memset(&iwdg, 0, sizeof iwdg), memset(&scb, 0, sizeof scb);
    gpioa.MODER = 0xEBFFFFFFu, gpioa.PUPDR = 0x24000000u;      /* reset values */
    moder_last = gpioa.MODER, pupdr_last = gpioa.PUPDR;
    rcc_flags = S->csr, rcc_csr_shown = 0, apbrstr2_last = 0, lsion = 0, lsi_on_at = 0;
    usart_reset_set = usart_reset_pulsed = 0, odr = 0, gpio_writes = 0;
    systick_ctrl = systick_shown = 0, ms_seen = 0;
    iwdg_last_refresh = 0, iwdg_update_until = 0, iwdg_started = iwdg_unlocked = iwdg_seq = 0;
    wd_counting = S->wd_run, wd_pending = 0, wd_fire_ns = S->wd_left_ns;
    wd_pr = wd_pr_new = iwdg.PR = iwdg_pr_last = S->wd_pr;
    wd_rlr = wd_rlr_new = iwdg.RLR = iwdg_rlr_last = S->wd_rlr;
    irq_on = 1;
    swd_head = swd_tail = 0, swd_frames = 0;
#ifdef REAL_FLASH_DRV
    drv_model_init(drv_advance, S->drv_sr, S->drv_bsy_stuck, S->drv_wrperr);
#endif
}

static void boot_results_clear(void) {
    S->outcome = 0, S->t_end = 0, S->violation[0] = 0;
    S->requests = S->header_req = S->off0_req = 0;
    memset(S->by_mode, 0, sizeof S->by_mode);
    S->first_req_ns = S->max_req_gap_ns = S->last_req_ns = 0;
    S->mid_frame = 0, S->answers = 0;
    memset(S->req0, 0, sizeof S->req0);
    memset(S->res, 0, sizeof S->res);
    S->rx_lost = S->pages = S->max_write_end = S->flash_calls = 0;
    S->iwdg_max_gap = 0, S->iwdg_running = 0, S->jump_valid = 0, S->led_n = 0;
    S->trace_first_gpio = S->trace_ok = 0, S->wd_fired = 0, S->wd_reload_ns = 0;
}

/* Both builds share the scenarios; each runs those its flash suits (table at the end). */
#define SCENARIO static __attribute__((unused)) void

#define EXPECT(c, ...)                                                                   \
    do {                                                                                 \
        if (!(c)) {                                                                      \
            g_fail++;                                                                    \
            printf("  FAIL [%s] ", g_scen);                                              \
            printf(__VA_ARGS__);                                                         \
            printf("  (%s, %s:%d)\n", #c, __FILE__, __LINE__);                           \
        }                                                                                \
    } while (0)

static const char *oname(int o) {
    switch (o) {
    case OUT_JUMP: return "app";
    case OUT_RESET: return "reset";
    case OUT_CUT: return "power cut";
    case OUT_TIMEOUT: return "still in the bootloader";
    case OUT_WDT: return "reset (RAM kept)";
    case OUT_BAD: return "BROKE A RULE";
    case OUT_RETURNED: return "main RETURNED";
    default: return "CRASHED";
    }
}

/* A power-up: the IWDG stopped, its registers at their reset values. */
static void wd_power_up(void) {
    S->wd_run = 0, S->wd_pr = 0, S->wd_rlr = 0xFFFu, S->wd_left_ns = 0;
}

/* The app that ran set the IWDG to this period and has just reloaded it when it resets (the
 * IWDG keeps running over the reset, the pessimistic reading of B8). */
static void app_watchdog(uint32_t pr, uint32_t rlr) {
    S->wd_run = 1, S->wd_pr = pr, S->wd_rlr = rlr, S->wd_left_ns = rlr * wd_tick_ns(pr);
}

/* One boot from reset. After it, the reset flags of the next boot are those the bootloader
 * leaves (its own reset adds SFTRSTF; nobody cleared the flags), the ZTU is up, and the checks
 * that hold for every boot are made. */
static int boot_once(void) {
    boot_results_clear();
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        child_init();
        Reset_Handler();
        finish(OUT_RETURNED);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int o = WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    S->outcome = o;
    if (o == OUT_RESET) {
        S->csr |= RCC_CSR_SFTRSTF;
    } else if (o == OUT_WDT) {
        S->csr |= RCC_CSR_IWDGRSTF;
        S->reset_at_ns = 0;
    } else if (o == OUT_JUMP && S->wd_run) {
        app_watchdog(3, 1000);        /* our apps' period, reloaded right before they reset */
    } else if (o == OUT_CUT) {
        wd_power_up();
    }
    S->ready_ns = 0;
    EXPECT(o == OUT_JUMP || o == OUT_RESET || o == OUT_CUT || o == OUT_TIMEOUT || o == OUT_WDT,
           "boot ended: %s (%d) %s\n", oname(o), o, S->violation);
    EXPECT(!S->violation[0], "%s\n", S->violation);
    EXPECT(!memcmp(FLASH_MEM, boot_sector, sizeof boot_sector), "sector 0 changed\n");
    EXPECT(!memcmp(FLASH_MEM + 0x3800 + 0x1000, settings_top, sizeof settings_top), "settings above 14 KB changed\n");
    EXPECT(S->mid_frame == 0, "%u requests started while a frame was half received\n", S->mid_frame);
    EXPECT(!S->iwdg_running || S->iwdg_max_gap <= GAP_LIMIT_NS, "watchdog gap %.1f ms\n", S->iwdg_max_gap / 1e6);
    if (S->iwdg_running && S->iwdg_max_gap > S->worst_gap) {
        S->worst_gap = S->iwdg_max_gap;
    }
    if (o == OUT_JUMP) {
        EXPECT(S->jump_valid, "jumped into an invalid app\n");
        EXPECT(S->trace_ok == 0x3FFFu, "state at the jump: checks %04x of 3fff\n", S->trace_ok);
    }
    if (o == OUT_JUMP || o == OUT_RESET) {
        EXPECT(BOOT_WORD_VALID(MB->boot, MB->boot_check) && !(MB->boot & BOOT_PROBE_REQ),
               "boot word %08x / %08x\n", MB->boot, MB->boot_check);
        EXPECT(MB->magic == 0 && MB->magic_inv == 0, "mailbox magic left\n");
    }
    return o;
}

/* ---- images, the app model, the ZTU's configuration -------------------------------------------- */

typedef struct { char name[24]; uint8_t d[IMG_MAX]; uint32_t n; } img_t;
static img_t g_img[40];
static int   g_imgs;

static const img_t *img(const char *name) {
    for (int i = 0; i < g_imgs; i++) {
        if (!strcmp(g_img[i].name, name)) {
            return &g_img[i];
        }
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.bin", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    img_t *m = &g_img[g_imgs++];
    snprintf(m->name, sizeof m->name, "%s", name);
    memset(m->d, 0xFF, IMG_MAX);
    m->n = (uint32_t)fread(m->d, 1, IMG_MAX, f);
    fclose(f);
    return m;
}

static uint32_t img_crc(const char *name) {
    return rd32le(img(name)->d + 0xC8);
}

static void flash_set(const char *name) {
    memset(FLASH_MEM + 0x1000, 0xFF, BOOT_APP_MAX);
    memset(FLASH_MEM + (HIDDEN_BASE - BOOT_BASE), 0xFF, HIDDEN_SIZE);
    if (name) {
        const img_t *m = img(name);
        memcpy(FLASH_MEM + 0x1000, m->d, m->n < BOOT_APP_MAX ? m->n : BOOT_APP_MAX);
        if (m->n > BOOT_APP_MAX) {
            memcpy(FLASH_MEM + (HIDDEN_BASE - BOOT_BASE), m->d + BOOT_APP_MAX, m->n - BOOT_APP_MAX);
        }
    }
}

static void flash_erase_app(void) {
    flash_set(NULL);
}

static int flash_is(const char *name) {
    const img_t   *m = img(name);
    static uint8_t d[BOOT_IMAGE_MAX];
    installed_bytes(d);
    return !memcmp(d, m->d, m->n);
}

static unsigned flash_build(void) {
    return FLASH_MEM[0x10CE] | FLASH_MEM[0x10CF] << 8;
}

static void carry(const char *name) {
    memset(S->img, 0xFF, IMG_MAX);
    memcpy(S->img, img(name)->d, img(name)->n);
    S->img_size = img(name)->n;
}

static void carry2(const char *name) {
    memset(S->img2, 0xFF, IMG_MAX);
    memcpy(S->img2, img(name)->d, img(name)->n);
    S->img2_size = img(name)->n;
}

/* A fresh scenario: an own ZTU serving 64-byte blocks, answering 2-5 ms after a request. */
static void fresh(void) {
    uint64_t worst = S->worst_gap;
    memset(S, 0, sizeof *S);
    S->worst_gap      = worst;
    S->ztu            = ZTU_OWN;
    S->gate           = GATE_RAW;
    S->serving        = 1;
    S->block_max      = OTA_ZTU_BLOCK_MAX;
    S->switch_at_page = S->stop_at_page = S->cut_at_page = S->bad_page = S->flaky_page = S->corrupt_off = -1;
    S->seed           = 12345;
    S->limit_ns       = 600 * S_;
    wd_power_up();
    memset(RAM_MEM, 0, 0x1000);
}

/* Mains on after a cut: random RAM, PWRRSTF (+ PINRSTF), the ZTU restarts too (its failure
 * pause ends, it answers from its boot time). */
static void cold(void) {
    for (int i = 0; i < 0x1000; i++) {
        RAM_MEM[i] = (uint8_t)rnd();
    }
    S->csr      = RCC_CSR_PWRRSTF | RCC_CSR_PINRSTF;
    S->failures = 0;
    S->ready_ns = S->ready_cfg_ns;
    wd_power_up();
}

/* The app ran and cleared the reset flags; a confirming run cleared the tries. */
static void app_ran(int confirms) {
    S->csr = 0;
    uint32_t w = MB->boot;
    if (confirms && BOOT_WORD_VALID(w, MB->boot_check)) {
        w &= ~BOOT_TRIES_MASK;
        MB->boot       = w;
        MB->boot_check = ~w;
    }
}

/* ... then it crashed (watchdog or fault reset). */
static void crash(void) {
    S->csr = RCC_CSR_IWDGRSTF;
}

/* ... then it accepted an offer of the carried image and restarted (src/tuya.c). */
static void app_accepts(void) {
    MB->magic      = OTA_MAILBOX_MAGIC;
    MB->magic_inv  = ~OTA_MAILBOX_MAGIC;
    MB->expect_crc = rd32le(S->img + 0xC8);
    S->csr         = RCC_CSR_SFTRSTF;
}

/* ... then its link watchdog fired (section 9, item 11): tries 0 and the probe request. */
static void app_link_restart(void) {
    uint32_t w     = (MB->boot & ~BOOT_TRIES_MASK) | BOOT_PROBE_REQ;
    MB->boot       = w;
    MB->boot_check = ~w;
    S->csr         = RCC_CSR_SFTRSTF;
}

static void word_set(uint32_t w) {
    MB->boot       = w;
    MB->boot_check = ~w;
}

static unsigned tries(void) {
    return MB->boot & BOOT_TRIES_MASK;
}

static unsigned probe_ms(void) {
    return (MB->boot & BOOT_PROBE_MS_MASK) >> BOOT_PROBE_MS_SHIFT;
}

static unsigned modes(void) {
    unsigned m = 0;
    for (int i = 0; i < 8; i++) {
        m |= (S->by_mode[i] ? 1u : 0u) << i;
    }
    return m;
}

static double secs(void) {
    return S->t_end / 1e9;
}

/* Boots until the app runs (an update resets once or more), at most n boots. */
static int boot_to_app(int n) {
    int o = 0;
    for (int b = 0; b < n; b++) {
        o = boot_once();
        if (o != OUT_RESET) {
            break;
        }
    }
    return o;
}

/* LED: total on-time and the number of on-phases within [a, b) of the last boot. */
static void led_stats(uint64_t a, uint64_t b, uint64_t *on_ns, unsigned *phases) {
    *on_ns  = 0;
    *phases = 0;
    for (uint32_t i = 0; i < S->led_n; i++) {
        uint64_t s = S->led[i].t, e = i + 1 < S->led_n ? S->led[i + 1].t : S->t_end;
        if (!S->led[i].on || e <= a || s >= b) {
            continue;
        }
        if (s < a) {
            s = a;
        }
        if (e > b) {
            e = b;
        }
        *on_ns += e - s;
        (*phases)++;
    }
}

/* ---- scenarios: update ------------------------------------------------------------------------ */

SCENARIO sc_asked(void) {
    fresh();
    flash_set("n300");
    carry("n301");
    cold();
    app_ran(1);
    app_accepts();
    int o = boot_once();
    printf("asked update, 64-byte blocks: %s after %.2f s, %u requests, result ok %u, build %u, modes (a bit each) "
           "%x\n", oname(o), secs(), S->requests, S->res[0], S->res_build, modes());
    EXPECT(o == OUT_RESET && S->res[0] == 1 && S->res_build == 301 && S->res_version == 0x9F, "update done\n");
    EXPECT(modes() == 1u << OTA_MODE_ASKED && S->header_req >= 1, "header first, mode 1 only\n");
    EXPECT(secs() > 1.5 && secs() < 4.0, "time %.2f s (design: 2.52 s)\n", secs());
    EXPECT((MB->boot & BOOT_INFO_WRITTEN) && tries() == 0, "WRITTEN, tries 0\n");
    o = boot_once();
    printf("  next boot: %s after %.3f s, build %u, tries %u, requests %u\n", oname(o), secs(), flash_build(), tries(),
           S->requests);
    EXPECT(o == OUT_JUMP && flash_is("n301") && tries() == 1 && S->requests == 0 && secs() < 0.1,
           "the new app at once, no probe, tries 1\n");

    fresh();                          /* 128-byte blocks */
    flash_set("n300");
    carry("n301");
    S->block_max = 128;
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    printf("asked update, 128-byte blocks: %s after %.2f s, %u requests\n", oname(o), secs(), S->requests);
    EXPECT(o == OUT_RESET && S->res[0] == 1 && S->requests < 110 && secs() < 3.0, "128-byte blocks\n");
    EXPECT(boot_once() == OUT_JUMP && flash_is("n301"), "new app\n");

    fresh();                          /* the ZTU never answers: the old app after the give-up */
    flash_set("n300");
    carry("n301");
    S->serving = 0;
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    printf("asked, ZTU silent: %s after %.2f s, %u requests, tries %u\n", oname(o), secs(), S->requests, tries());
    EXPECT(o == OUT_JUMP && secs() > 9.9 && secs() < 10.6 && flash_is("n300") && S->flash_calls == 0 && tries() == 1,
           "give-up after 10 s\n");

    fresh();                          /* the app accepted a CRC the ZTU does not carry */
    flash_set("n300");
    carry("n301");
    cold();
    app_ran(1);
    app_accepts();
    MB->expect_crc = img_crc("n302");
    o = boot_once();
    EXPECT(o == OUT_JUMP && flash_is("n300") && S->off0_req == 0 && S->flash_calls == 0,
           "another CRC than the asked one is never taken\n");
}

static void cuts(int mode) {
    int bad = 0, n = 0;
    {
        for (long k = 0; k < 100; k++) {
            fresh();
            flash_set("n299");
            carry("n301");
            cold();
            app_ran(1);
            app_accepts();
            S->cut_at_page       = k;
            S->cut_after_program = mode;
            int o = boot_once();
            if (o != OUT_CUT) {
                continue;
            }
            n++;
            S->cut_at_page = -1;
            cold();
            S->limit_ns = 120 * S_;
            o = boot_to_app(5);
            if (o != OUT_JUMP || !flash_is("n301")) {
                printf("  cut %s page %ld: %s, image %s\n", mode ? "after program" : "after erase", k, oname(o),
                       flash_is("n301") ? "301" : "other");
                bad++;
            }
        }
    }
    printf("power cut after the %s of every page of an asked update: %d of %d not recovered\n",
           mode ? "program" : "erase", bad, n);
    EXPECT(bad == 0 && n >= 90, "power cuts\n");
}

SCENARIO sc_cuts_erase(void) {
    cuts(0);
}

SCENARIO sc_cuts_program(void) {
    cuts(1);
}

SCENARIO sc_switch(void) {
    fresh();
    flash_set("n300");
    carry("n301");
    carry2("n302");
    S->switch_at_page = 3;
    cold();
    app_ran(1);
    app_accepts();
    int o = boot_once();
    printf("image change mid-transfer (asked 301, the ZTU reboots into 302 at page 3): %s after %.1f s, build %u\n",
           oname(o), secs(), flash_build());
    EXPECT(o == OUT_RESET && flash_is("n302") && secs() < 20, "the new image after the give-up\n");
    EXPECT(boot_once() == OUT_JUMP && flash_is("n302"), "302 runs\n");

    fresh();                          /* the same during a probe: unpinned, no app, any image */
    flash_set("n300");
    carry("n301");
    carry2("n299");
    S->switch_at_page = 10;
    cold();
    o = boot_to_app(3);
    printf("  during a probe update (301, then 299 from page 10): %s, build %u\n", oname(o), flash_build());
    EXPECT(o == OUT_JUMP && flash_is("n299"), "probe image change\n");
}

/* Item 28: the ZTU serves the header and page 0, which is the same in 300 and 301, then nothing.
 * The page was offered to the writer (so g_intact is cleared) but nothing was erased: the give-up
 * must check the app again and start it, not take it for a half-written one. */
SCENARIO sc_stallp0(void) {
    fresh();
    flash_set("n300");
    carry("n301");
    S->serve_below = 128;
    cold();
    app_ran(1);
    app_accepts();
    int o = boot_once();
    printf("ZTU answers the header and page 0 (identical), then nothing: %s after %.1f s, %u offset-0 requests, %u "
           "page writes offered, %u erased, old app intact %d\n", oname(o), secs(), S->off0_req, S->flash_calls,
           S->pages, flash_is("n300"));
    EXPECT(S->header_req >= 1 && S->off0_req >= 1 && S->flash_calls >= 1 && S->pages == 0,
           "pinned, page 0 offered to the writer, nothing erased\n");
    EXPECT(o == OUT_JUMP && flash_is("n300") && secs() > 9.9 && secs() < 11, "old app after the give-up\n");
}

/* Section 8, Receive (item 39(4)): after a bad checksum, or a length no answer of ours has, the
 * buffered bytes are scanned again before more are read. Every answer comes right behind a cut
 * frame (PREFIX_*), so it is half in the buffer when the cut frame fails: without the rescan
 * every answer is lost. */
SCENARIO sc_rescan(void) {
    const char *what[] = { "no cut frame", "a frame with a bad checksum", "a header cut after 6 bytes" };
    uint32_t    base   = 0;
    for (int k = PREFIX_NONE; k <= PREFIX_LONG; k++) {
        fresh();                      /* the probe, nothing newer: the first answer ends it */
        flash_set("n300");
        carry("n300");
        S->prefix = k;
        cold();
        int      op = boot_once();
        double   tp = secs();
        unsigned hr = S->header_req, pm = probe_ms();
        EXPECT(op == OUT_JUMP && hr == 1 && pm > 0 && pm < 20 && tp < 0.2,
               "%s: the probe used the first answer\n", what[k]);

        fresh();                      /* an asked update: every answer used, nothing asked again */
        flash_set("n300");
        carry("n301");
        S->prefix = k;
        cold();
        app_ran(1);
        app_accepts();
        int o = boot_once();
        if (k == PREFIX_NONE) {
            base = S->requests;
        }
        printf("answers behind %-28s: probe %s after %.3f s (%u header request, answer %u ms); asked update %s "
               "after %.2f s, %u requests\n", what[k], oname(op), tp, hr, pm, oname(o),
               secs(), S->requests);
        EXPECT(o == OUT_RESET && S->res[0] == 1 && S->requests == base && secs() < 4.0,
               "%s: the update with no answer lost\n", what[k]);
        EXPECT(boot_once() == OUT_JUMP && flash_is("n301"), "%s: new app\n", what[k]);
    }
}

SCENARIO sc_noisy(void) {
    int fails = 0;
    double tot = 0, mx = 0;
    for (int b128 = 0; b128 < 2; b128++) {
        for (int r = 0; r < 50; r++) {
            fresh();
            flash_set("n299");
            carry("n301");
            S->block_max = b128 ? 128 : 64;
            S->seed = 1000u + (unsigned)r;
            S->drop_p = 0.05, S->dup_p = 0.05, S->flip_p = 0.02;
            cold();
            app_ran(1);
            app_accepts();
            int o = boot_once();
            if (o != OUT_RESET || !flash_is("n301")) {
                fails++;
                printf("  run %d: %s\n", r, oname(o));
            }
            if (!b128) {
                tot += secs();
                mx = secs() > mx ? secs() : mx;
            }
        }
    }
    printf("noisy link (5%% dropped, 5%% late duplicates, 2%% bit flips), 50 seeds x 64/128-byte blocks: %d failed; "
           "64-byte mean %.1f s, max %.1f s\n", fails, tot / 50, mx);
    EXPECT(fails == 0 && mx < 60, "noisy\n");
}

SCENARIO sc_unsol(void) {
    unsigned per[] = { 1000, 300, 100, 50 };
    for (int i = 0; i < 4; i++) {
        fresh();
        flash_set("n299");
        carry("n301");
        S->unsol_every_ms = per[i];
        cold();
        app_ran(1);
        app_accepts();
        int o = boot_once();
        printf("unsolicited ZTU frames every %4u ms during an update: %s after %.2f s, %u requests, RX bytes lost %u\n",
               per[i], oname(o), secs(), S->requests, S->rx_lost);
        EXPECT(o == OUT_RESET && flash_is("n301"), "unsolicited frames\n");
    }
}

SCENARIO sc_flashfail(void) {
    fresh();
    flash_set("n300");
    carry("n301");
    S->bad_page = 41;                 /* a page that differs between 300 and 301 */
    S->limit_ns = 60 * S_;
    cold();
    app_ran(1);
    app_accepts();
    int o = boot_once();
    printf("page 41 never verifies: %s after 60 s, FLASH results %u (ZTU paused), %u requests\n", oname(o),
           S->results_flash, S->requests);
    EXPECT(o == OUT_TIMEOUT && S->results_flash == 3 && S->requests < 600 && S->requests > 100,
           "FLASH results, the ZTU pauses, no tight loop\n");
    EXPECT(S->max_req_gap_ns < 600 * MS, "requests go on at the pace: largest gap %.0f ms\n", S->max_req_gap_ns / 1e6);
}

/* Section 8, flash driver: each page is tried twice before OTA_RESULT_FLASH. Page 41 (it differs
 * between 300 and 301) fails its first write only: the second attempt writes it, nothing is
 * reported, nothing is fetched again. */
SCENARIO sc_flashretry(void) {
    fresh();
    flash_set("n300");
    carry("n301");
    cold();
    app_ran(1);
    app_accepts();
    boot_once();
    uint32_t base_pages = S->pages, base_req = S->requests;
    fresh();
    flash_set("n300");
    carry("n301");
    S->flaky_page = 41;
    cold();
    app_ran(1);
    app_accepts();
    int o = boot_once();
    printf("page 41 fails its first write only: %s after %.2f s, FLASH results %u, page writes %u (%u without), %u "
           "requests (%u without)\n", oname(o), secs(), S->res[2], S->pages, base_pages, S->requests, base_req);
    EXPECT(o == OUT_RESET && S->res[0] == 1 && S->res[2] == 0 && S->results_flash == 0 && S->flaky_page == -1 &&
           S->pages == base_pages + 1 && S->requests == base_req, "the second attempt, no FLASH result\n");
    EXPECT(boot_once() == OUT_JUMP && flash_is("n301"), "new app, page 41 included\n");
}

SCENARIO sc_badvec(void) {
    const char *bad[] = { "badsp301", "badrv301" };
    for (int k = 0; k < 2; k++) {
        fresh();
        flash_set("n300");
        carry(bad[k]);
        cold();
        int o = boot_once();
        printf("%s carried (CRC valid, newer), power-up: %s after %.2f s, BAD_IMAGE %u, pages written %u\n", bad[k],
               oname(o), secs(), S->res[1], S->pages);
        EXPECT(o == OUT_JUMP && S->res[1] == 1 && S->flash_calls == 0 && flash_is("n300"), "refused at page 0\n");
        int took = 0;
        for (int b = 0; b < 6; b++) {
            S->limit_ns = 120 * S_;
            if (boot_once() == OUT_RESET) {
                took = 1;
            }
            crash();
        }
        EXPECT(!took && S->pages_total == 0 && flash_is("n300"), "%s: never in a crash loop\n", bad[k]);
        fresh();                      /* the app even accepted it */
        flash_set("n300");
        carry(bad[k]);
        cold();
        app_ran(1);
        app_accepts();
        o = boot_once();
        EXPECT(o == OUT_JUMP && S->res[1] == 1 && S->flash_calls == 0 && flash_is("n300") && secs() < 11,
               "%s: asked, refused at page 0, old app after the give-up\n", bad[k]);
    }
}

/* ---- scenarios: probe ------------------------------------------------------------------------- */

SCENARIO sc_probe(void) {
    struct {
        const char *inst, *carried;
        long        ready_ms;
        int         serving, update;
        const char *what;
    } c[] = {
        { "n300", "n301", 300, 1, 1, "newer build" },
        { "n300", "n300", 300, 1, 0, "same image" },
        { "n300", "n299", 300, 1, 0, "older build" },
        { "n300", "v2_1_12", 300, 1, 0, "old image without build (2.1.12)" },
        { "v2_1_10", "n301", 300, 1, 1, "installed 2.1.10 (no build), carried 301" },
        { "v2_1_10", "v2_1_11", 300, 1, 0, "both without build" },
        { "n300", "n301", 0, 0, 0, "ZTU silent" },
        { "n300", "n301", 1400, 1, 1, "ZTU ready at 1.4 s" },
        { "n300", "n301", 2900, 1, 1, "ZTU ready at 2.9 s" },
        { "n300", "n301", 3200, 1, 0, "ZTU ready at 3.2 s (after the window)" },
        { "n300", "sim306", 300, 1, 0, "carried SIM image" },
        { "n300", "diag307", 300, 1, 0, "carried DIAG image, newer build" },
        { "n300", "measure308", 300, 1, 0, "carried MEASURE image, newer build" },
        { "n300", "magic301", 300, 1, 0, "carried header without APDZ" },
        { "n300", "bffff", 300, 1, 0, "carried build 0xFFFF" },
        { "n300", "big400", 300, 1, 0, "13 KB over a 12 KB-ring app" },
        { "n303r", "big400", 300, 1, 1, "13 KB over a RING_HIGH app" },
        { "n300", "max304", 300, 1, 1, "exactly 12 KB over a 12 KB-ring app" },
        { "n300", "over305", 300, 1, 0, "12 KB + 4 over a 12 KB-ring app" },
        { "n303r", "max406r", 300, 1, 1, "exactly 14 KB over a RING_HIGH app" },
        { "n303r", "over407r", 300, 1, 1, "14 KB + 4 over a RING_HIGH app (v3: hidden)" },
        { "n303r", "hid408r", 300, 1, 1, "18 KB over a RING_HIGH app" },
        { "n303r", "hmax411r", 300, 1, 1, "exactly 26 KB over a RING_HIGH app" },
        { "n303r", "hover412r", 300, 1, 0, "26 KB + 4 over a RING_HIGH app" },
        { "n300", "hid408r", 300, 1, 0, "18 KB over a 12 KB-ring app" },
        { "hid408r", "hid409r", 300, 1, 1, "18 KB over 18 KB (hidden pages proven)" },
        { "diag305", "n301", 300, 1, 0, "installed DIAG build: no probe" },
        { "measure308", "n301", 300, 1, 0, "installed MEASURE build: no probe" },
        { "sim306", "n301", 300, 1, 0, "installed SIM build: no probe" },
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        fresh();
        flash_set(c[i].inst);
        carry(c[i].carried);
        S->ready_cfg_ns = (uint64_t)c[i].ready_ms * MS;
        S->serving      = c[i].serving;
        cold();
        int      o  = boot_once();
        double   t1 = secs();
        unsigned hr = S->header_req, pm = probe_ms(), led_on;
        uint64_t led_ns;
        led_stats(0, S->t_end, &led_ns, &led_on);
        uint32_t pages = S->pages;
        int      updated = o == OUT_RESET;
        if (updated) {
            o = boot_once();
        }
        printf("probe, %-42s: %-11s first boot %.2f s, header asks %u, answer %4u ms, then %s%s\n", c[i].what,
               updated ? "updated," : "untouched,", t1, hr, pm, oname(o),
               updated && S->requests ? " (PROBED AGAIN)" : "");
        EXPECT(updated == c[i].update && o == OUT_JUMP, "case %u\n", i);
        EXPECT(!updated || (S->requests == 0 && flash_is(c[i].carried) && tries() == 1),
               "after the update: no probe\n");
        EXPECT(updated || (flash_is(c[i].inst) && pages == 0), "untouched\n");
        if (strstr(c[i].what, "no probe")) {
            EXPECT(hr == 0 && t1 < 0.1, "no request for a bench build\n");
        } else {
            EXPECT(hr >= 1 && (c[i].serving && c[i].ready_ms < 3000 ? pm > 0 : pm == 0), "answer time\n");
            EXPECT(S->by_mode[OTA_MODE_PROBE] || updated, "mode 3\n");
        }
        if (!updated && c[i].serving && c[i].ready_ms < 3000 && !strstr(c[i].what, "no probe")) {
            EXPECT(t1 < c[i].ready_ms / 1000.0 + 0.2, "ends at the first answer\n");
        }
        if (!c[i].serving || c[i].ready_ms >= 3000) {
            EXPECT(t1 > 3.0 && t1 < 3.2, "the window: %.2f s\n", t1);
        }
        EXPECT(updated || led_on == 0, "LED off in the probe\n");
    }
    /* a probe that takes an image: LED off until the first block, then the fast blink */
    fresh();
    flash_set("n300");
    carry("n301");
    S->ready_cfg_ns = 300 * MS;
    cold();
    int      o = boot_once();
    uint64_t on;
    unsigned ph;
    led_stats(0, 300 * MS, &on, &ph);
    EXPECT(o == OUT_RESET && on == 0, "LED off before the answer\n");
    led_stats(600 * MS, S->t_end - 100 * MS, &on, &ph);
    double span = (S->t_end - 700 * MS) / 1e9;
    printf("probe update: LED off for the first 0.3 s, then %u on-phases in %.1f s (fast blink 128/128 ms)\n", ph,
           span);
    EXPECT(ph >= span / 0.256 - 2 && ph <= span / 0.256 + 2, "fast blink while writing\n");
}

SCENARIO sc_probecoll(void) {
    int lat[] = { 20, 50, 97, 99, 100, 101, 103, 150, 199, 200, 201 };
    int worst = 20;
    for (int i = 0; i < 11; i++) {
        int got = 0;
        for (int r = 0; r < 20; r++) {
            fresh();
            flash_set("n300");
            carry("n301");
            S->seed         = 77u + (unsigned)r;
            S->probe_lat_ms = lat[i];
            S->limit_ns     = 120 * S_;
            cold();
            if (boot_once() == OUT_RESET) {
                got++;
            }
        }
        worst = got < worst ? got : worst;
        if (got < 20) {
            printf("  latency %d ms: newer build found in %d of 20 power-ups\n", lat[i], got);
        }
    }
    printf("probe, ZTU answers 20..201 ms (+-1.5 ms) after each request: the newer build found in at least %d of 20 "
           "power-ups at every latency\n", worst);
    EXPECT(worst == 20, "probe collisions\n");
}

SCENARIO sc_probedelay(void) {
    long r[] = { 0, 100, 300, 600, 1000, 1400, 2000, 2900, 3200, -1 };
    printf("power-up to app start, nothing newer carried, by ZTU ready time:");
    for (int i = 0; i < 10; i++) {
        fresh();
        flash_set("n300");
        carry("n300");
        if (r[i] < 0) {
            S->serving = 0;
        } else {
            S->ready_cfg_ns = (uint64_t)r[i] * MS;
        }
        cold();
        int o = boot_once();
        printf(" %s%.2f s", r[i] < 0 ? "silent " : "", secs());
        if (r[i] >= 0) {
            printf(" (%ld ms);", r[i]);
        }
        EXPECT(o == OUT_JUMP, "jump\n");
        if (r[i] >= 0 && r[i] < 3000) {
            EXPECT(secs() >= r[i] / 1000.0 && secs() < r[i] / 1000.0 + 0.16, "delay = answer + ~50 ms\n");
        } else {
            EXPECT(secs() > 3.0 && secs() < 3.2, "the window\n");
        }
    }
    printf("\n");
}

/* B3: the probe does not depend on the app's update path: an app that declines every offer stays
 * through warm resets and is replaced at the next power-up. */
SCENARIO sc_probe_b3(void) {
    fresh();
    flash_set("n300");
    carry("n301");
    cold();
    S->serving = 0;                   /* nothing at this power-up */
    int o = boot_once();
    S->serving = 1;
    for (int b = 0; b < 3; b++) {     /* runs, confirms, declines the offer, a reset now and then */
        app_ran(1);
        crash();
        o = boot_once();
        EXPECT(o == OUT_JUMP && flash_is("n300") && S->requests == 0, "warm: no probe\n");
    }
    cold();
    o = boot_to_app(3);
    printf("B3: an app with a broken update path, then a power cut: %s, build %u\n", oname(o), flash_build());
    EXPECT(o == OUT_JUMP && flash_is("n301"), "the probe took the newer build\n");
}

/* ---- scenarios: rescue, button, counter ------------------------------------------------------- */

SCENARIO sc_crashloop(void) {
    fresh();
    flash_set("n302");
    carry("n302");
    cold();
    int got = 0;
    char line[16] = "";
    for (int b = 1; b <= 10; b++) {
        if (b == 7) {
            carry("n299");
        }
        S->limit_ns = 120 * S_;
        MB->restored = 0;
        int o = boot_once();
        int w = o == OUT_JUMP && secs() > 29;
        line[b - 1] = o == OUT_RESET ? 'U' : w ? 'W' : '.';
        if (w) {
            EXPECT(secs() < 31 && S->off0_req == 0 && S->flash_calls == 0 && MB->restored == OTA_RESTORED_MAGIC &&
                   (MB->boot & BOOT_INFO_RESCUE) && S->by_mode[OTA_MODE_RESCUE] == S->requests,
                   "a window: 30 s, same image skipped, restored marked, mode 4\n");
        }
        if (o == OUT_RESET) {
            got = b;
            EXPECT((MB->boot & (BOOT_INFO_RESCUE | BOOT_INFO_WRITTEN)) == (BOOT_INFO_RESCUE | BOOT_INFO_WRITTEN) &&
                   MB->restored == OTA_RESTORED_MAGIC, "RESCUE and WRITTEN\n");
            o = boot_once();
            EXPECT(o == OUT_JUMP && flash_is("n299") && tries() == 1 && S->requests == 0, "the older image runs\n");
            break;
        }
        crash();
    }
    printf("crash loop of 302 (ZTU carries 302, from boot 7 an older 299): [%s] installed at boot %d\n", line, got);
    EXPECT(!strcmp(line, "...WWWU") && got == 7, "3 runs, then windows, then the older image\n");
}

/* A crash loop under an IWDG that a warm reset keeps running (the pessimistic reading of B8) with
 * a short period the crashing app chose: PR 0 / RLR 40, ~4.9 ms, far less than app_valid()
 * (section 4, Watchdog). watchdog_start() reloads after its update wait, so the bootloader's own
 * period is in effect before app_valid() and every run counts: the window opens at boot 4, as in
 * "crashloop", whether the app's watchdog ran out or the app faulted 3.9 ms after a reload.
 * Without that reload each warm boot ran out before it counted (the old code: ".R.R.RW"). */
SCENARIO sc_wdg_carry(void) {
    for (int fault = 0; fault <= 1; fault++) {
        fresh();
        flash_set("n302");
        carry("n302");
        cold();
        char line[16] = "";
        uint64_t worst_reload = 0;
        for (int b = 1; b <= 10; b++) {
            S->limit_ns = 120 * S_;
            int o = boot_once();
            int w = o == OUT_JUMP && secs() > 29;
            line[b - 1] = o == OUT_WDT ? 'R' : o == OUT_RESET ? 'U' : w ? 'W' : '.';
            if (b > 1 && S->wd_reload_ns > worst_reload) {
                worst_reload = S->wd_reload_ns;
            }
            if (w) {
                EXPECT((MB->boot & BOOT_INFO_RESCUE) && S->by_mode[OTA_MODE_RESCUE] == S->requests && S->flash_calls == 0,
                       "a counter window, nothing written\n");
                break;
            }
            if (o == OUT_WDT) {
                continue;                          /* ran out in the bootloader: boots again */
            }
            if (o != OUT_JUMP) {
                break;
            }
            app_watchdog(0, 40);
            if (fault) {
                S->csr        = RCC_CSR_SFTRSTF;   /* HardFault_Handler: a system reset at once */
                S->wd_left_ns = 1 * MS;
            } else {
                crash();                           /* its watchdog ran out: from RLR again */
            }
        }
        printf("crash loop, IWDG kept at PR 0 / RLR 40 (~4.9 ms) over each reset, the app %s: [%s], "
               "own period reloaded %.2f ms after reset at the latest\n", fault ? "faults" : "watchdog runs out", line,
               worst_reload / 1e6);
        EXPECT(!strcmp(line, "...W"), "3 counted runs, then the window\n");
        EXPECT(worst_reload > 0 && worst_reload < 2 * MS, "reloaded at the end of watchdog_start()\n");
    }
}

SCENARIO sc_bigrescue(void) {
    fresh();
    flash_set("n302");
    carry("big400");
    cold();
    app_ran(1);
    word_set(BOOT_WORD_FRESH | 3);
    crash();
    S->limit_ns = 120 * S_;
    int o = boot_once();
    printf("counter rescue, 12 KB-ring app, 13 KB carried: %s after %.1f s, pages written %u\n", oname(o), secs(),
           S->pages);
    EXPECT(o == OUT_JUMP && S->flash_calls == 0 && flash_is("n302"), "refused\n");
    fresh();
    flash_set("n302");
    carry("big400");
    S->button = 1, S->button_release_ns = 500 * MS;
    cold();
    o = boot_once();
    EXPECT(o == OUT_JUMP && S->flash_calls == 0 && flash_is("n302"), "button: refused too\n");
    fresh();
    flash_set("n303r");
    carry("big400");
    cold();
    app_ran(1);
    word_set(BOOT_WORD_FRESH | 3);
    crash();
    o = boot_once();
    EXPECT(o == OUT_RESET && S->max_write_end == APP_BASE + 13312u, "RING_HIGH app: taken\n");
}

SCENARIO sc_benchrescue(void) {
    const char *b[] = { "sim306", "diag307", "measure308" };
    for (int k = 0; k < 3; k++) {
        fresh();
        flash_set("n302");
        carry(b[k]);
        cold();
        int took = 0;
        for (int i = 0; i < 6; i++) {
            S->limit_ns = 120 * S_;
            if (boot_once() == OUT_RESET) {
                took = 1;
            }
            crash();
        }
        fresh();
        flash_set("n302");
        carry(b[k]);
        S->button = 1, S->button_release_ns = 300 * MS;
        cold();
        int o1 = boot_once();
        fresh();
        flash_erase_app();
        carry(b[k]);
        S->limit_ns = 30 * S_;
        cold();
        int o2 = boot_once();
        printf("ZTU carries %-10s: crash loop installed %d; button: %s; no app: %s after 30 s, pages %u\n", b[k], took,
               oname(o1), oname(o2), S->pages);
        EXPECT(!took && o1 == OUT_JUMP && o2 == OUT_TIMEOUT && S->flash_calls == 0, "bench images never taken\n");
    }
}

/* A window wrote part of an older image, then the ZTU went back to the installed one: with no
 * valid app the give-up drops the skip, so even the image that crashed is taken. */
SCENARIO sc_rescue_noapp(void) {
    fresh();
    flash_set("n302");
    carry("n299");
    carry2("n302");
    S->switch_at_page = 10;
    cold();
    app_ran(1);
    word_set(BOOT_WORD_FRESH | 3);
    crash();
    S->limit_ns = 120 * S_;
    int o = boot_to_app(3);
    printf("window wrote 10 pages of 299, then the ZTU carries 302 again: %s, build %u\n", oname(o), flash_build());
    EXPECT(o == OUT_JUMP && flash_is("n302"), "no app: the skipped image too\n");
}

/* NO_APP after a give-up (section 4, the state machine; section 8, give-up): unpin, unskip,
 * unrefuse, and the 14 KB limit (section 1: recovery comes before settings). rescue-noapp pins the
 * unskip; these the limit and the refusal. */
SCENARIO sc_giveup_noapp(void) {
    fresh();                          /* the limit: an asked update under a 12 KB-ring app wrote 4 pages,
                                         then the ZTU carries the 13 KB big400 */
    flash_set("n300");
    carry("n301");
    carry2("big400");
    S->switch_at_page = 3;
    cold();
    app_ran(1);
    app_accepts();
    int o = boot_once();
    printf("asked 301 over a 12 KB-ring app, 4 pages written, then 13 KB carried: %s after %.1f s, mode-2 requests "
           "%u, written up to %08x\n", oname(o), secs(), S->by_mode[OTA_MODE_NO_APP], S->max_write_end);
    EXPECT(o == OUT_RESET && S->by_mode[OTA_MODE_NO_APP] > 0 && S->res[0] == 1 && S->res_build == 400 &&
           S->max_write_end == APP_BASE + 13312u, "no app after the give-up: 14 KB\n");
    o = boot_once();
    EXPECT(o == OUT_JUMP && flash_is("big400"), "big400 runs\n");

    fresh();                          /* the refusal: no app, page 0 corrupted in transit once */
    flash_erase_app();
    carry("n301");
    S->corrupt_off = 7;               /* the reset vector's top byte: 0x48001101, outside the image */
    S->limit_ns    = 60 * S_;
    cold();
    o = boot_once();
    printf("no app, page 0 of 301 corrupted once: %s after %.1f s, BAD_IMAGE %u, offset-0 requests %u\n", oname(o),
           secs(), S->res[1], S->off0_req);
    EXPECT(o == OUT_RESET && S->res[1] == 1 && S->res[0] == 1 && S->off0_req == 2 && secs() > 10 && secs() < 16,
           "refused for the run, taken again after the give-up\n");
    o = boot_once();
    EXPECT(o == OUT_JUMP && flash_is("n301"), "301 runs\n");
}

/* Bench builds (section 2): an installed one leaves by a button rescue (or SWD); a carried one is
 * never taken in any mode. Asked mode here; probe, benchrescue, link and gates have the others. */
SCENARIO sc_benchmodes(void) {
    const char *inst[] = { "diag305", "sim306", "measure308" };
    for (int k = 0; k < 3; k++) {
        fresh();
        flash_set(inst[k]);
        carry("n301");
        S->button = 1, S->button_release_ns = 500 * MS;
        cold();
        int o = boot_once();
        printf("button with %-10s installed, 301 carried: %s after %.2f s, mode-5 requests %u, info %03x\n", inst[k],
               oname(o), secs(), S->by_mode[OTA_MODE_BUTTON], MB->boot & 0xF00u);
        EXPECT(o == OUT_RESET && S->res[0] == 1 && S->requests && S->by_mode[OTA_MODE_BUTTON] == S->requests &&
               (MB->boot & BOOT_INFO_BUTTON), "%s: a button rescue\n", inst[k]);
        S->button = 0;
        o = boot_once();
        EXPECT(o == OUT_JUMP && flash_is("n301"), "%s: 301 runs\n", inst[k]);
    }
    const char *carried[] = { "diag307", "sim306", "measure308" };
    for (int k = 0; k < 3; k++) {
        fresh();
        flash_set("n300");
        carry(carried[k]);
        cold();
        app_ran(1);
        app_accepts();                /* the mailbox holds the bench image's CRC */
        int o = boot_once();
        printf("asked update of the carried %-10s: %s after %.2f s, %u header requests, page writes offered %u\n",
               carried[k], oname(o), secs(), S->header_req, S->flash_calls);
        EXPECT(o == OUT_JUMP && S->header_req >= 1 && S->off0_req == 0 && S->flash_calls == 0 && flash_is("n300") &&
               secs() > 9.9 && secs() < 10.6, "%s: never taken, even asked\n", carried[k]);
    }
}

/* The image limit per mode (section 1): asked follows the installed header's RING_HIGH; with no
 * valid app it is 14 KB at once, also when the broken app's header has no RING_HIGH. */
SCENARIO sc_limits(void) {
    fresh();
    flash_set("n300");
    carry("big400");
    cold();
    app_ran(1);
    app_accepts();
    int o = boot_once();
    printf("asked update of the 13 KB big400 over a 12 KB-ring app: %s after %.2f s, page writes offered %u\n",
           oname(o), secs(), S->flash_calls);
    EXPECT(o == OUT_JUMP && S->header_req >= 1 && S->flash_calls == 0 && flash_is("n300") && secs() > 9.9,
           "12 KB ring: refused\n");

    fresh();
    flash_set("n303r");
    carry("big400");
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    printf("asked update of the 13 KB big400 over a RING_HIGH app: %s after %.2f s, written up to %08x\n", oname(o),
           secs(), S->max_write_end);
    EXPECT(o == OUT_RESET && S->res[0] == 1 && S->max_write_end == APP_BASE + 13312u, "RING_HIGH: taken\n");
    EXPECT(boot_once() == OUT_JUMP && flash_is("big400"), "big400 runs\n");

    fresh();                          /* 300's header intact (flags 0x08: no RING_HIGH), page 50 damaged */
    flash_set("n300");
    FLASH_MEM[0x1000 + 50 * 128] ^= 0x01;
    carry("big400");
    cold();
    o = boot_once();
    printf("no valid app (header without RING_HIGH, CRC bad), 13 KB carried: %s after %.2f s, written up to %08x, "
           "modes %x\n", oname(o), secs(), S->max_write_end, modes());
    EXPECT(o == OUT_RESET && secs() < 6 && S->max_write_end == APP_BASE + 13312u && modes() == 1u << OTA_MODE_NO_APP,
           "no app: 14 KB from the start, no give-up needed\n");
    EXPECT(boot_once() == OUT_JUMP && flash_is("big400"), "big400 runs\n");
}

SCENARIO sc_rescuecut(void) {
    int bad = 0, n = 0;
    for (long k = 0; k < 90; k += 7) {
        fresh();
        flash_set("n302");
        carry("n299");
        S->button = 1, S->button_release_ns = 300 * MS;
        S->cut_at_page = k;
        cold();
        if (boot_once() != OUT_CUT) {
            continue;
        }
        n++;
        S->button = 0, S->cut_at_page = -1;
        carry("n301");
        cold();
        S->limit_ns = 120 * S_;
        int o = boot_to_app(5);
        if (o != OUT_JUMP || !flash_is("n301")) {
            bad++;
            printf("  cut %ld: %s build %u\n", k, oname(o), flash_build());
        }
    }
    printf("power cut while a button rescue writes, then the ZTU carries 301: %d of %d not recovered\n", bad, n);
    EXPECT(bad == 0 && n > 10, "rescue cuts\n");
}

SCENARIO sc_duty(void) {
    fresh();
    flash_set("n302");
    carry("n302");
    cold();
    double listen = 0, total = 0;
    for (int b = 0; b < 12; b++) {
        S->limit_ns = 120 * S_;
        boot_once();
        listen += secs();
        total  += secs() + 1.0;       /* + 1 s in the hanging app until the watchdog */
        crash();
    }
    printf("app hanging in init, 12 boots: %.0f %% of the time listening for an image\n", 100 * listen / total);
    EXPECT(listen / total > 0.9, "duty\n");
}

SCENARIO sc_restored(void) {
    fresh();                          /* counter rescue: marked */
    flash_set("n302");
    carry("n301");
    cold();
    app_ran(1);
    MB->restored = 0;
    word_set(BOOT_WORD_FRESH | 3);
    crash();
    int o = boot_once();
    printf("counter rescue (tries 3), ZTU carries 301: %s after %.2f s, restored %08x\n", oname(o), secs(),
           MB->restored);
    EXPECT(o == OUT_RESET && MB->restored == OTA_RESTORED_MAGIC, "restored marked\n");
    o = boot_once();
    EXPECT(o == OUT_JUMP && flash_is("n301") && tries() == 1 && (MB->boot & BOOT_INFO_WRITTEN), "new app\n");
    /* every other path leaves the restore words alone */
    struct { const char *what; int kind; } c[] = {
        { "plain warm boot", 0 }, { "button rescue", 1 }, { "power-on probe", 2 }, { "asked update", 3 },
        { "link probe", 4 },
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        fresh();
        flash_set("n302");
        carry("n301");
        cold();
        if (c[i].kind == 1) {
            S->button = 1, S->button_release_ns = 300 * MS;
        } else if (c[i].kind != 2) {
            app_ran(1);
            crash();
        }
        if (c[i].kind == 3) {
            app_accepts();
        }
        if (c[i].kind == 4) {
            word_set(BOOT_WORD_FRESH);
            app_link_restart();
        }
        MB->restore_check = 0x11111111u, MB->restore = 0x22222222u, MB->restored = 0x33333333u;
        boot_to_app(3);
        EXPECT(MB->restore_check == 0x11111111u && MB->restore == 0x22222222u && MB->restored == 0x33333333u,
               "%s: restore words untouched\n", c[i].what);
    }
}

SCENARIO sc_button(void) {
    fresh();
    flash_set("n302");
    carry("n299");
    S->button = 1, S->button_release_ns = 500 * MS;
    cold();
    int      o = boot_once();
    uint64_t on;
    unsigned ph;
    led_stats(0, 500 * MS, &on, &ph);
    printf("button released at 0.5 s, ZTU carries an older 299: %s after %.2f s, info %03x\n", oname(o), secs(),
           MB->boot & 0xF00u);
    EXPECT(o == OUT_RESET && (MB->boot & BOOT_INFO_BUTTON) && !(MB->boot & BOOT_INFO_RESCUE) && S->by_mode[5] > 0 &&
           secs() < 5, "button rescue\n");
    EXPECT(on > 380 * MS && on <= 400 * MS + 1 * MS && ph == 1, "LED on from 0.1 s until the release (%.0f ms)\n",
           on / 1e6);
    o = boot_once();                  /* still held, but warm */
    printf("  next boot, button still held: %s after %.3f s, build %u\n", oname(o), secs(), flash_build());
    EXPECT(o == OUT_JUMP && flash_is("n299") && secs() < 0.1, "a held button at a warm boot is ignored\n");

    fresh();                          /* same image: the window, then the app */
    flash_set("n302");
    carry("n302");
    S->button = 1, S->button_release_ns = 500 * MS;
    cold();
    o = boot_once();
    led_stats(1 * S_, 29 * S_, &on, &ph);
    printf("button, ZTU carries the same image: %s after %.2f s, offset-0 requests %u, LED on %.0f %% in %u phases\n",
           oname(o), secs(), S->off0_req, 100.0 * on / (28 * S_), ph);
    EXPECT(o == OUT_JUMP && secs() > 30.3 && secs() < 31.0 && S->off0_req == 0 && S->flash_calls == 0, "30 s window\n");
    EXPECT(ph >= 26 && ph <= 29 && on > 13 * S_ && on < 15 * S_, "slow blink 512/512 ms\n");

    fresh();                          /* released after 4.5 s: still a rescue */
    flash_set("n302");
    carry("n299");
    S->button = 1, S->button_release_ns = 4500 * MS;
    cold();
    o = boot_once();
    EXPECT(o == OUT_RESET && (MB->boot & BOOT_INFO_BUTTON), "released at 4.5 s\n");

    fresh();                          /* never released: stuck, no rescue, the normal boot after 5 s */
    flash_set("n302");
    carry("n299");
    S->button = 1, S->button_release_ns = 0;
    cold();
    o = boot_once();
    printf("button stuck, older image carried: %s after %.2f s, info %03x, pages %u\n", oname(o), secs(),
           MB->boot & 0xF00u, S->pages);
    EXPECT(o == OUT_JUMP && flash_is("n302") && (MB->boot & BOOT_INFO_STUCK) && !(MB->boot & BOOT_INFO_BUTTON) &&
           secs() > 5.0 && secs() < 5.3 && S->by_mode[OTA_MODE_PROBE] > 0, "stuck\n");

    fresh();                          /* a press shorter than 100 ms: nothing */
    flash_set("n302");
    carry("n299");
    S->button = 1, S->button_release_ns = 60 * MS;
    cold();
    o = boot_once();
    EXPECT(o == OUT_JUMP && !(MB->boot & 0xF00u) && secs() < 0.5, "bounce ignored\n");

    fresh();                          /* held at a warm boot: ignored */
    flash_set("n302");
    carry("n299");
    cold();
    app_ran(1);
    crash();
    S->button = 1;
    o = boot_once();
    EXPECT(o == OUT_JUMP && S->requests == 0 && secs() < 0.1, "warm: ignored\n");

    fresh();                          /* not held: no delay */
    flash_set("n302");
    carry("n302");
    cold();
    o = boot_once();
    printf("no button, same image carried: %s after %.2f s\n", oname(o), secs());
    EXPECT(o == OUT_JUMP && secs() < 0.4, "no delay\n");
}

/* A row of boots: '.' a quick start, 'W' a rescue window, 'U' an update. */
static void row(char *line, int n, int confirms, int stock_rule) {
    for (int b = 0; b < n; b++) {
        S->limit_ns = 120 * S_;
        int o = boot_once();
        line[b] = o == OUT_RESET ? 'U' : secs() > 29 ? 'W' : '.';
        app_ran(confirms || stock_rule);
        crash();
    }
    line[n] = 0;
}

SCENARIO sc_counter(void) {
    char line[16];
    fresh();
    flash_set("v2_1_12");
    carry("v2_1_12");
    cold();
    row(line, 9, 0, 0);
    printf("2.1.12 (no CONFIRMS), never confirms, 1 cold + 8 warm boots: [%s] tries %u\n", line, tries());
    EXPECT(!strcmp(line, ".........") && tries() == 0, "never counted\n");

    fresh();
    flash_set("diag305");
    carry("n301");
    cold();
    row(line, 9, 0, 0);
    printf("DIAG build with CONFIRMS, never confirms: [%s] tries %u\n", line, tries());
    EXPECT(!strcmp(line, ".........") && tries() == 0, "bench builds never counted\n");

    fresh();
    flash_set("n300");
    carry("n300");
    cold();
    row(line, 11, 1, 0);
    printf("confirming app, a reset after every confirmation: [%s]\n", line);
    EXPECT(!strcmp(line, "..........."), "never a window\n");

    fresh();
    flash_set("n300");
    carry("n300");
    cold();
    row(line, 10, 0, 0);
    printf("CONFIRMS app, never confirmed (a ZTU that never asks 0xA3), 1 cold + 9 warm: [%s]\n", line);
    EXPECT(!strcmp(line, "...WWWWWWW"), "every warm reset after the third\n");

    fresh();                          /* an asked update resets the tries */
    flash_set("n300");
    carry("n301");
    cold();
    app_ran(1);
    word_set(BOOT_WORD_FRESH | 2);
    app_accepts();
    S->serving = 0;
    int o = boot_once();
    EXPECT(o == OUT_JUMP && tries() == 1, "asked: tries 0, then 1 at the jump\n");

    fresh();                          /* stale PWRRSTF next to IWDGRSTF: warm */
    flash_set("n300");
    carry("n301");
    cold();
    app_ran(1);
    word_set(BOOT_WORD_FRESH | 1);
    S->csr = RCC_CSR_PWRRSTF | RCC_CSR_IWDGRSTF;
    o = boot_once();
    EXPECT(o == OUT_JUMP && tries() == 2 && S->requests == 0, "PWR + IWDG counts as warm\n");

    fresh();                          /* the boot word's other bits are not tries */
    flash_set("n300");
    carry("n300");
    cold();
    app_ran(1);
    word_set(BOOT_WORD_FRESH | BOOT_INFO_LINK | BOOT_RESERVED_BITS | 2u);
    crash();
    o = boot_once();
    EXPECT(o == OUT_JUMP && secs() < 0.1 && tries() == 3 && (MB->boot & BOOT_INFO_LINK), "bits 7..4 are not tries\n");
    o = boot_once();
    EXPECT(o == OUT_JUMP && secs() > 29, "then the window at tries 3\n");
}

/* ---- scenarios: link watchdog hook ---------------------------------------------------------- */

SCENARIO sc_link(void) {
    struct {
        const char *inst, *carried;
        int         serving, update;
        const char *what;
    } c[] = {
        { "n300", "n301", 1, 1, "newer build carried" },
        { "n300", "n300", 1, 0, "same image" },
        { "n300", "n299", 1, 0, "older build" },
        { "n300", "v2_1_12", 1, 0, "carried 2.1.12 without a build" },
        { "n300", "n301", 0, 0, "ZTU does not answer 0xA1" },
        { "n300", "sim306", 1, 0, "carried SIM image" },
        { "n300", "diag307", 1, 0, "carried DIAG image" },
        { "n300", "big400", 1, 0, "13 KB image over a 12 KB-ring app" },
        { "n303r", "big400", 1, 1, "13 KB image over a RING_HIGH app" },
        { "diag305", "n301", 1, 0, "installed DIAG build" },
        { "measure308", "n301", 1, 0, "installed MEASURE build" },
        { "sim306", "n301", 1, 0, "installed SIM build" },
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        fresh();
        flash_set(c[i].inst);
        carry(c[i].inst);
        cold();
        int o = boot_once();          /* power-up: the probe finds nothing newer */
        EXPECT(o == OUT_JUMP, "cold boot\n");
        app_ran(1);                   /* it ran 4 h and confirmed; then the link died */
        word_set(MB->boot | (0x5A5u << BOOT_PROBE_MS_SHIFT));   /* a stale probe time */
        carry(c[i].carried);
        S->serving = c[i].serving;
        app_link_restart();
        MB->restore_check = 0x11111111u, MB->restore = 0x22222222u, MB->restored = 0x33333333u;
        o = boot_once();
        double t1 = secs();
        unsigned lr = S->by_mode[OTA_MODE_LINK], pm = probe_ms(), led_on;
        uint64_t led_ns;
        led_stats(0, S->t_end, &led_ns, &led_on);
        int updated = o == OUT_RESET;
        int bench   = strstr(c[i].what, "installed") != NULL;
        printf("link probe, %-34s: %-11s %.2f s, mode-6 asks %u, answer %u ms, word %06x\n", c[i].what,
               updated ? "updated," : "untouched,", t1, lr, pm, MB->boot & 0xFFFFFFu);
        EXPECT(MB->restore_check == 0x11111111u && MB->restore == 0x22222222u && MB->restored == 0x33333333u,
               "restore words untouched\n");
        EXPECT(S->requests == lr && !(MB->boot & (BOOT_INFO_RESCUE | BOOT_INFO_BUTTON)), "mode 6 only, no rescue\n");
        if (bench) {
            EXPECT(o == OUT_JUMP && S->requests == 0 && t1 < 0.1 && !(MB->boot & BOOT_INFO_LINK),
                   "no probe for a bench build, request cleared\n");
            continue;
        }
        EXPECT((MB->boot & BOOT_INFO_LINK) && lr > 0, "a link probe ran\n");
        EXPECT(updated == c[i].update, "taken if and only if newer and it fits\n");
        EXPECT(updated || (o == OUT_JUMP && flash_is(c[i].inst) && S->flash_calls == 0), "untouched\n");
        EXPECT(c[i].serving ? pm > 0 && pm < 100 : pm == 0 && t1 > 3.0 && t1 < 3.2, "probe time replaced\n");
        EXPECT(c[i].serving || (lr >= 28 && lr <= 31), "a request every 100 ms for 3 s\n");
        EXPECT(updated || tries() == 1, "a probe, not counted against the app: tries 1\n");
        EXPECT(updated || led_on == 0, "LED off\n");
        if (updated) {
            o = boot_once();
            printf("  next boot: %s after %.3f s, build %u, tries %u, info %03x, LINK %d\n", oname(o), secs(),
                   flash_build(), tries(), MB->boot & 0xF00u, !!(MB->boot & BOOT_INFO_LINK));
            EXPECT(o == OUT_JUMP && S->requests == 0 && tries() == 1 && (MB->boot & BOOT_INFO_WRITTEN) &&
                   (MB->boot & BOOT_INFO_LINK) && flash_is(c[i].carried), "after the link update: no second probe\n");
        }
    }
}

SCENARIO sc_link_order(void) {
    fresh();                          /* a request + an asked update: the asked update */
    flash_set("n300");
    carry("n301");
    cold();
    boot_once();
    app_ran(1);
    app_link_restart();
    app_accepts();
    int o = boot_once();
    printf("link request + asked update: %s, modes %x\n", oname(o), modes());
    EXPECT(o == OUT_RESET && modes() == 1u << OTA_MODE_ASKED && !(MB->boot & BOOT_INFO_LINK), "asked first\n");

    fresh();                          /* a request with tries 3: the counter rescue */
    flash_set("n302");
    carry("n299");
    cold();
    boot_once();
    app_ran(1);
    word_set(BOOT_WORD_FRESH | 3u | BOOT_PROBE_REQ);
    MB->restored = 0;
    crash();
    o = boot_once();
    printf("link request with tries 3: %s after %.1f s, modes %x, info %03x\n", oname(o), secs(), modes(),
           MB->boot & 0xF00u);
    EXPECT(o == OUT_RESET && modes() == 1u << OTA_MODE_RESCUE && (MB->boot & BOOT_INFO_RESCUE) &&
           MB->restored == OTA_RESTORED_MAGIC, "counter rescue first\n");

    fresh();                          /* a cold boot never sees a request: mode 3, LINK clear */
    flash_set("n300");
    carry("n300");
    cold();
    o = boot_once();
    EXPECT(o == OUT_JUMP && S->by_mode[OTA_MODE_PROBE] > 0 && !S->by_mode[OTA_MODE_LINK] &&
           !(MB->boot & BOOT_INFO_LINK), "cold: a power-on probe\n");
    /* a stale request in RAM at a power-up */
    flash_set("n300");
    for (int i = 0; i < 20; i++) {
        cold();
        word_set(BOOT_WORD_FRESH | BOOT_PROBE_REQ);
        o = boot_once();
        EXPECT(o == OUT_JUMP && !S->by_mode[OTA_MODE_LINK] && !(MB->boot & BOOT_INFO_LINK), "stale request, cold\n");
    }

    fresh();                          /* a request in an invalid word: no probe */
    flash_set("n300");
    carry("n301");
    cold();
    boot_once();
    app_ran(1);
    MB->boot |= BOOT_PROBE_REQ;       /* complement not updated */
    S->csr = RCC_CSR_SFTRSTF;
    o = boot_once();
    EXPECT(o == OUT_JUMP && S->requests == 0 && secs() < 0.1 && !(MB->boot & BOOT_INFO_LINK),
           "invalid word: no probe\n");

    fresh();                          /* LINK set: a crash loop still opens its first window at the 4th boot */
    flash_set("n302");
    carry("n302");
    cold();
    boot_once();
    app_ran(1);
    app_link_restart();
    o = boot_once();
    EXPECT(o == OUT_JUMP && (MB->boot & BOOT_INFO_LINK) && tries() == 1, "link probe\n");
    char line[16];
    crash();
    row(line, 6, 0, 0);
    printf("after a link probe (LINK set), crash loop: [%s]\n", line);
    EXPECT(!strcmp(line, "..WWWW"), "windows from the 4th unconfirmed run only\n");

    fresh();                          /* 10 link restarts under a ZTU that never answers, never confirmed */
    flash_set("n300");
    carry("n301");
    S->serving = 0, S->unsol_every_ms = 1000;
    cold();
    boot_once();
    int windows = 0, slow = 0, written = 0;
    double mx = 0;
    MB->restored = 0x12345678u;
    for (int b = 0; b < 10; b++) {
        app_ran(0);
        app_link_restart();
        o = boot_once();
        written += o != OUT_JUMP;
        windows += secs() > 29;
        slow += secs() > 3.2;
        mx = secs() > mx ? secs() : mx;
    }
    printf("10 link restarts, ZTU never answers 0xA1, foreign frames every 1 s, never confirmed: %d windows, longest "
           "%.2f s, tries %u\n", windows, mx, tries());
    EXPECT(windows == 0 && slow == 0 && written == 0 && MB->restored == 0x12345678u && tries() == 1,
           "link restarts never open a window\n");
}

/* ---- scenarios: mailbox ----------------------------------------------------------------------- */

SCENARIO sc_ram(void) {
    int bad = 0;
    fresh();
    flash_set("n300");
    carry("n300");
    for (int r = 0; r < 200; r++) {
        cold();
        uint8_t keep[12];
        memcpy(keep, (const void *)&MB->restore_check, 8);
        memcpy(keep + 8, (const void *)&MB->restored, 4);
        int o = boot_once();
        if (o != OUT_JUMP || S->by_mode[OTA_MODE_ASKED] || S->by_mode[OTA_MODE_LINK] || S->off0_req || tries() != 1 ||
            (MB->boot & 0xF0u) || memcmp(keep, (const void *)&MB->restore_check, 8) ||
            memcmp(keep + 8, (const void *)&MB->restored, 4)) {
            bad++;
        }
    }
    printf("random RAM at power-up, 200 boots: %d wrong (asked, link, word, restore words)\n", bad);
    EXPECT(bad == 0, "random RAM\n");
}

SCENARIO sc_modes(void) {
    unsigned seen[7] = { 0 };
    fresh();                          /* 3: probe */
    flash_set("n300");
    carry("n301");
    cold();
    S->serving = 0;
    boot_once();
    seen[3] = modes();
    S->serving = 1;
    app_ran(1);                       /* 1: asked */
    app_accepts();
    S->serving = 0;
    boot_once();
    seen[1] = modes();
    app_ran(1);                       /* 6: link */
    app_link_restart();
    boot_once();
    seen[6] = modes();
    app_ran(0);                       /* 4: counter rescue */
    word_set(BOOT_WORD_FRESH | 3);
    crash();
    S->limit_ns = 60 * S_;
    boot_once();
    seen[4] = modes();
    S->button = 1, S->button_release_ns = 200 * MS;   /* 5: button */
    cold();
    boot_once();
    seen[5] = modes();
    S->button = 0;
    flash_erase_app();                /* 2: no app */
    cold();
    S->limit_ns = 5 * S_;
    boot_once();
    seen[2] = modes();
    printf("request modes: asked %x, no app %x, probe %x, rescue %x, button %x, link %x\n", seen[1], seen[2], seen[3],
           seen[4], seen[5], seen[6]);
    for (int m = 1; m <= 6; m++) {
        EXPECT(seen[m] == 1u << m, "mode %d\n", m);
    }
}

/* The request frame byte for byte, built as docs/BOOTLOADER.md section 5 builds its example
 * (55 AA 02 00 00 A1 00 10 | 00 00 00 C0 00 10 03 03 <installed header bytes 8..15> | sum), for
 * the installed image of this run (byte 14: the bootloader version, 3 since v3). */
SCENARIO sc_request_bytes(void) {
    const char *inst[] = { "n300", "v2_1_12", "n303r" };
    for (int k = 0; k < 3; k++) {
        fresh();
        flash_set(inst[k]);
        carry("n300");
        S->serving = 0;
        cold();
        boot_once();
        uint8_t want[OTA_REQ_LEN + 9] = { 0x55, 0xAA, 0x02, 0x00, 0x00, OTA_CMD_BLOCK, 0x00, 0x10,
                                          0x00, 0x00, 0x00, 0xC0, 0x00, 0x10, BOOT_VERSION, 0x03 };
        memcpy(want + 16, img(inst[k])->d + 0xC8, 8);
        for (int i = 0; i < 24; i++) {
            want[24] = (uint8_t)(want[24] + want[i]);
        }
        printf("first request with %-8s installed:", inst[k]);
        for (int i = 0; i < 25; i++) {
            printf(" %02X", S->req0[i]);
        }
        printf("\n");
        EXPECT(!memcmp(S->req0, want, sizeof want), "the request bytes of section 5\n");
    }
    fresh();                          /* the result frame: status, version, build (BE) */
    flash_set("n300");
    carry("n301");
    cold();
    boot_once();
    EXPECT(S->res[0] == 1 && S->res_version == img("n301")->d[0xCC] && S->res_build == 301, "result v2\n");
}

/* The probe took a newer image and wrote it, the image failed its check (wrong data under good
 * frame checksums), and the ZTU then carries an older one: the probe's refusal must not start
 * the half-written app; with no valid app, any image is taken. */
SCENARIO sc_probe_refused_after_writes(void) {
    fresh();
    flash_set("n300");
    carry("n301");
    carry2("n299");
    S->corrupt_off   = 5000;
    S->switch_on_bad = 1;
    cold();
    int o = boot_to_app(4);
    printf("probe wrote 301, it failed its check, the ZTU then carries 299: %s, BAD_IMAGE %u, build %u, modes %x\n",
           oname(o), S->results_bad, flash_build(), modes());
    EXPECT(o == OUT_JUMP && flash_is("n299") && S->results_bad == 1, "no half app; any image then\n");
    EXPECT(probe_ms() > 0 && probe_ms() < 50, "the probe time is the first answer's: %u ms\n", probe_ms());

    /* the same with 302 first and then 301: 301 is newer than the build installed at this boot
     * (300), though page 1 already holds 302's header: the probe takes it, as a probe */
    fresh();
    flash_set("n300");
    carry("n302");
    carry2("n301");
    S->corrupt_off   = 5000;
    S->switch_on_bad = 1;
    cold();
    o = boot_once();
    printf("probe wrote 302, it failed, the ZTU then carries 301: %s, modes %x\n", oname(o), modes());
    EXPECT(o == OUT_RESET && modes() == 1u << OTA_MODE_PROBE, "compared with the build installed at this boot\n");
    EXPECT(boot_once() == OUT_JUMP && flash_is("n301"), "301 runs\n");
}

/* Frame contents a ZTU must never produce, and resets in the middle of a boot. */
SCENARIO sc_robust(void) {
    fresh();                          /* an image whose real CRC is 0, newer: "no image" inside */
    flash_set("n300");
    carry("crc0");
    cold();
    int o = boot_once();
    printf("carried image with CRC 0 (newer build): %s after %.2f s, pages %u\n", oname(o), secs(), S->pages);
    EXPECT(o == OUT_JUMP && secs() < 0.5 && S->flash_calls == 0, "CRC 0 refused at the header\n");

    fresh();                          /* block header size disagrees with the image's header */
    flash_set("n300");
    carry("n301");
    S->size_skew = -128;
    cold();
    o = boot_once();
    printf("ZTU tells a size 128 bytes off the image header's: %s after %.2f s, pages %u\n", oname(o), secs(),
           S->pages);
    EXPECT(o == OUT_JUMP && secs() < 0.5 && S->flash_calls == 0, "inconsistent header refused\n");

    fresh();                          /* every block answer at the wrong offset: paced, not a tight loop */
    flash_set("n300");
    carry("n301");
    S->offset_skew = 64;
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    printf("ZTU answers every block request with another offset: %s after %.1f s, %u requests\n", oname(o), secs(),
           S->requests);
    EXPECT(o == OUT_JUMP && flash_is("n300") && S->requests < 250 && S->requests > 150,
           "stray blocks asked again after 50 ms (~20 a second)\n");

    fresh();                          /* CRC 0 in a rescue that already refused an image at page 0 */
    flash_set("n302");
    carry("badsp301");
    carry2("crc0");
    S->switch_on_bad = 1;             /* the ZTU reboots into the CRC-0 image after the refusal */
    cold();
    app_ran(1);
    word_set(BOOT_WORD_FRESH | 3);
    crash();
    S->limit_ns = 120 * S_;
    o = boot_once();
    printf("rescue window: a bad-SP image refused at page 0, then a CRC-0 image: %s after %.1f s\n", oname(o), secs());
    /* the taken header was progress: GIVE_UP_MS from it (section 8), and then the window ends */
    EXPECT(o == OUT_JUMP && secs() > 9.5 && secs() < 11 && flash_is("n302"), "the window still ends\n");

    fresh();                          /* a ZTU that sends more than asked (contract item 1 broken) */
    flash_set("n299");                /* every page differs, the last one too */
    carry("n301");
    S->ignore_max_len = 1;
    S->limit_ns = 30 * S_;
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    printf("ZTU ignores max_len (64 bytes also past the image end): %s, results ok %u bad %u, %u requests in 30 s\n",
           oname(o), S->res[0], S->res[1], S->requests);
    EXPECT(o == OUT_TIMEOUT && S->res[0] == 0 && S->res[1] == 0 && S->requests < 1000,
           "a block reaching past the image end is never taken (stray, paced)\n");

    fresh();                          /* a reset (RAM kept) during a link probe: the request is gone */
    flash_set("n300");
    carry("n301");
    S->serving = 0;
    cold();
    boot_once();
    app_ran(1);
    app_link_restart();
    S->reset_at_ns = 1 * S_;
    o = boot_once();
    EXPECT(o == OUT_WDT, "reset in the probe\n");
    o = boot_once();
    printf("reset 1 s into a link probe: the next boot %s after %.3f s, %u requests\n", oname(o), secs(), S->requests);
    EXPECT(o == OUT_JUMP && S->requests == 0 && secs() < 0.1 && (MB->boot & BOOT_INFO_LINK),
           "the request never outlives the boot that read it\n");

    fresh();                          /* a reset during a counter window: the next boot opens one again */
    flash_set("n302");
    carry("n302");
    cold();
    boot_once();
    app_ran(1);
    word_set(BOOT_WORD_FRESH | 3);
    crash();
    S->reset_at_ns = 5 * S_;
    o = boot_once();
    o = boot_once();
    EXPECT(o == OUT_JUMP && secs() > 29 && (MB->boot & BOOT_INFO_RESCUE), "window again after a reset in one\n");
}

/* ---- scenarios: the stock Tuya ZTU (section 15) ----------------------------------------------- */

static void stock(int garbage) {
    fresh();
    S->ztu          = ZTU_STOCK;
    S->garbage      = garbage;
    S->ready_cfg_ns = 300 * MS;
}

static void stockztu(int garbage) {
    {
        const char *g = garbage ? " (9600-baud garbage phases)" : "";
        double lo = 1e9, hi = 0;
        unsigned rq_lo = 1000, rq_hi = 0, bad = 0;
        stock(garbage);
        flash_set("n300");
        for (int r = 0; r < 20; r++) {
            S->seed = 500u + (unsigned)r;
            cold();
            int o = boot_once();
            lo = secs() < lo ? secs() : lo, hi = secs() > hi ? secs() : hi;
            rq_lo = S->header_req < rq_lo ? S->header_req : rq_lo;
            rq_hi = S->header_req > rq_hi ? S->header_req : rq_hi;
            bad += o != OUT_JUMP || S->flash_calls || probe_ms() || S->by_mode[3] != S->requests || !flash_is("n300");
        }
        printf("stockztu-cold%s: 20 power-ups, app at %.2f..%.2f s, %u..%u header requests (mode 3), %u wrong\n", g, lo,
               hi, rq_lo, rq_hi, bad);
        EXPECT(bad == 0 && lo > 3.0 && hi < 3.2 && rq_lo >= 28 && rq_hi <= 31, "stockztu-cold\n");

        stock(garbage);
        flash_set("n300");
        S->button = 1, S->button_release_ns = 500 * MS;
        cold();
        int o = boot_once();
        printf("stockztu-button%s: %s after %.2f s, pages %u\n", g, oname(o), secs(), S->pages);
        EXPECT(o == OUT_JUMP && secs() > 30.3 && secs() < 31.0 && S->flash_calls == 0, "stockztu-button\n");

        for (int rule = 1; rule >= 0; rule--) {
            char line[16];
            stock(garbage);
            flash_set("n300");
            cold();
            MB->restored = 0x5A5A5A5Au;
            row(line, 11, 0, rule);
            printf("stockztu-counter%s, %s: 1 cold + 10 warm [%s]\n", g,
                   rule ? "the app confirms by the 0x01 rule" : "without that rule (the cost it avoids)", line);
            if (rule) {
                EXPECT(!strcmp(line, "...........") && MB->restored == 0x5A5A5A5Au, "0 windows\n");
            } else {
                EXPECT(!strcmp(line, "...WWWWWWWW"), "8 windows\n");
            }
        }

        stock(garbage);
        flash_set("n300");
        cold();
        boot_once();
        app_ran(1);
        app_link_restart();
        o = boot_once();
        printf("stockztu-link%s: %s after %.2f s, %u mode-6 requests, LINK %d, probe time %u\n", g, oname(o), secs(),
               S->by_mode[6], !!(MB->boot & BOOT_INFO_LINK), probe_ms());
        EXPECT(o == OUT_JUMP && secs() > 3.0 && secs() < 3.2 && S->by_mode[6] == S->requests && S->requests >= 28 &&
               S->flash_calls == 0 && (MB->boot & BOOT_INFO_LINK) && probe_ms() == 0, "stockztu-link\n");

        stock(garbage);
        flash_erase_app();
        S->limit_ns = 3600 * S_;
        cold();
        o = boot_once();
        double per = S->requests ? (S->last_req_ns - S->first_req_ns) / 1e6 / (S->requests - 1) : 0;
        printf("stockztu-noapp%s: 1 h without an app: %s, %u requests (one per %.0f ms, largest gap %.0f ms), RX bytes "
               "lost %u, largest watchdog gap %.1f ms\n", g, oname(o), S->requests, per, S->max_req_gap_ns / 1e6,
               S->rx_lost, S->iwdg_max_gap / 1e6);
        EXPECT(o == OUT_TIMEOUT && S->requests > 7000 && S->max_req_gap_ns < 1000 * MS && S->flash_calls == 0 &&
               S->by_mode[2] == S->requests, "stockztu-noapp\n");
    }
}

SCENARIO sc_stockztu(void) {
    stockztu(0);
}

SCENARIO sc_stockztu_garbage(void) {
    stockztu(1);
}

/* ---- fuzz ------------------------------------------------------------------------------------- */

static void fuzz(int part) {
    const char *start[3] = { "n300", "n302", NULL };
    int runs = 0, bad = 0;
    unsigned long answers = 0;
    for (int r = part * 100; r < part * 100 + 100; r++) {
        int m = r % 6;
        fresh();
        if (start[r % 3]) {
            flash_set(start[r % 3]);
        } else {
            flash_erase_app();
        }
        carry("n301");
        S->seed = 4242u + (unsigned)r;
        cold();
        if (m == 1) {
            app_ran(1);
            app_accepts();
        } else if (m == 2) {
            app_ran(1);
            word_set(BOOT_WORD_FRESH | 3);
            crash();
        } else if (m == 3) {
            S->button = 1, S->button_release_ns = 200 * MS;
        } else if (m == 4) {
            app_ran(1);
            word_set(BOOT_WORD_FRESH);
            app_link_restart();
        }
        S->fuzz = 1;
        S->limit_ns = 40 * S_;
        boot_once();
        runs++;
        answers += S->requests;
        S->fuzz = 0, S->button = 0;
        cold();
        S->limit_ns = 120 * S_;
        int o = boot_to_app(5);
        if (o != OUT_JUMP) {
            bad++;
            printf("  fuzz run %d mode %d: after a clean ZTU %s\n", r, m, oname(o));
        }
    }
    printf("fuzz, part %d: %d runs in every mode (~%lu requests answered with mutated or random frames), then a "
           "clean ZTU: %d without an intact app\n", part + 1, runs, answers, bad);
    EXPECT(bad == 0, "fuzz\n");
}

SCENARIO sc_fuzz1(void) {
    fuzz(0);
}

SCENARIO sc_fuzz2(void) {
    fuzz(1);
}

SCENARIO sc_fuzz3(void) {
    fuzz(2);
}

/* ---- phase-1 / phase-3 ZTU gates, SWD, the register trace, LSI ------------------------------------ */

SCENARIO sc_ztu_gates(void) {
    struct { int gate; const char *carried; int update; const char *what; } c[] = {
        { GATE_P1, "n301", 1, "phase-1 ZTU, normal image" },
        { GATE_P3, "n301", 1, "phase-3 ZTU, normal image" },
        { GATE_P1, "diag307", 0, "phase-1 ZTU serves DIAG; the bootloader refuses it" },
        { GATE_P3, "diag307", 0, "phase-3 ZTU does not serve DIAG" },
        { GATE_P1, "badsp301", 0, "phase-1 ZTU serves a bad SP; refused at page 0" },
        { GATE_P3, "badsp301", 0, "phase-3 ZTU does not serve a bad SP" },
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        fresh();
        S->gate = c[i].gate;
        flash_set("n300");
        carry(c[i].carried);
        cold();
        int o = boot_once();
        int updated = o == OUT_RESET;
        if (updated) {
            o = boot_once();
        }
        printf("%-52s: %s\n", c[i].what, updated ? "updated" : "untouched");
        EXPECT(updated == c[i].update && o == OUT_JUMP && S->pages_total == (updated ? S->pages_total : 0), "gate\n");
    }
}

/* v3 (section 17): an image over 14 KB continues in the hidden flash at 0x08005000, past the
 * settings ring (boot_once() checks the ring and sector 0 after every boot). */
SCENARIO sc_hidden(void) {
    fresh();
    flash_set("n303r");
    carry("hid408r");
    cold();
    app_ran(1);
    app_accepts();
    int o = boot_once();
    printf("asked update 11 KB -> 18 KB: %s after %.2f s, hidden page writes %u (32 tested twice, 32 written)\n",
           oname(o), secs(), S->hidden_writes);
    EXPECT(o == OUT_RESET && S->res[0] == 1, "taken\n");
#ifndef REAL_FLASH_DRV
    EXPECT(S->hidden_writes == 96, "tested, then written\n");
#endif
    EXPECT(boot_once() == OUT_JUMP && flash_is("hid408r"), "18 KB runs\n");

    fresh();                          /* 18 KB installed, 22 KB carried: only pages 32..63 tested */
    flash_set("hid408r");
    carry("hid410r");
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    printf("asked update 18 KB -> 22 KB: %s, hidden page writes %u (32 past the installed tested twice, 64 "
           "written)\n", oname(o), S->hidden_writes);
    EXPECT(o == OUT_RESET && S->res[0] == 1, "taken\n");
#ifndef REAL_FLASH_DRV
    EXPECT(S->hidden_writes == 128, "the installed app's pages kept\n");
#endif
    EXPECT(boot_once() == OUT_JUMP && flash_is("hid410r"), "22 KB runs\n");

    fresh();                          /* back to an image inside the app area */
    flash_set("hid410r");
    carry("n301");
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    EXPECT(o == OUT_RESET, "11 KB over 22 KB: taken\n");
#ifndef REAL_FLASH_DRV
    EXPECT(S->hidden_writes == 0, "no hidden write\n");
#endif
    EXPECT(boot_once() == OUT_JUMP && flash_is("n301"), "11 KB runs\n");

#ifndef REAL_FLASH_DRV
    fresh();                          /* a chip whose hidden flash never programs */
    flash_set("n303r");
    carry("hid408r");
    S->hidden_bad = 1;
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    printf("hidden flash that never programs, asked: %s after %.2f s, FLASH results %u, hidden page writes %u\n",
           oname(o), secs(), S->results_flash, S->hidden_writes);
    EXPECT(o == OUT_JUMP && S->results_flash == 1 && S->hidden_writes == 1 && flash_is("n303r"),
           "refused at the first hidden page, the app untouched, not tried again\n");

    fresh();
    flash_set("n303r");
    carry("hid408r");
    S->hidden_bad = 1;
    cold();
    o = boot_once();
    printf("hidden flash that never programs, probe: %s after %.2f s\n", oname(o), secs());
    EXPECT(o == OUT_JUMP && secs() < 1.0 && flash_is("n303r"), "the probe ends, the app starts\n");

    fresh();                          /* 18 KB installed, its pages work, the next ones do not */
    flash_set("hid408r");
    carry("hid410r");
    S->hidden_bad = 1 + 32;
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    EXPECT(o == OUT_JUMP && S->results_flash == 1 && flash_is("hid408r"),
           "22 KB refused at hidden page 32, the 18 KB app intact\n");

    fresh();                          /* a power cut while the hidden part is written */
    flash_set("n303r");
    carry("hid408r");
    cold();
    app_ran(1);
    app_accepts();
    boot_once();
    long whole = S->pages_total;
    fresh();
    flash_set("n303r");
    carry("hid408r");
    S->cut_at_page = whole - 8;
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    EXPECT(o == OUT_CUT, "cut at page write %ld of %ld\n", whole - 8, whole);
    cold();
    o = boot_once();
    printf("power cut at hidden page write %ld of %ld, then: %s after %.2f s, modes %x\n", whole - 8, whole,
           oname(o), secs(), modes());
    EXPECT(o == OUT_RESET && modes() == 1u << OTA_MODE_NO_APP, "no app: taken again\n");
    EXPECT(boot_once() == OUT_JUMP && flash_is("hid408r"), "18 KB runs\n");

    fresh();                          /* no app and a failing hidden flash: refused again and again */
    flash_erase_app();
    carry("hid408r");
    S->hidden_bad = 1;
    S->limit_ns = 35 * S_;
    cold();
    o = boot_once();
    printf("no app, hidden flash that never programs: %s after %.0f s, FLASH results %u\n", oname(o), secs(),
           S->results_flash);
    EXPECT(o == OUT_TIMEOUT && S->results_flash >= 2 && S->pages <= 2u * (uint32_t)S->results_flash,
           "never more than the tested page\n");
#endif
}

SCENARIO sc_swd(void) {
    fresh();
    S->ztu = ZTU_SWD;
    flash_set("n300");
    carry("n301");
    cold();
    app_ran(1);
    app_accepts();
    int o = boot_once();
    printf("SWD path (answers through g_inject, UART silent): %s after %.2f s, %u requests, %u frames injected, %u "
           "UART bytes\n", oname(o), secs(), S->requests, S->swd_frames, S->wire_bytes);
    EXPECT(o == OUT_RESET && S->res[0] == 1 && S->swd_frames > 150 && S->wire_bytes == 0, "swd update\n");
    EXPECT(boot_once() == OUT_JUMP && flash_is("n301"), "new app\n");
}

SCENARIO sc_regs(void) {
    fresh();
    flash_set("n300");
    carry("n300");
    cold();
    int o = boot_once();
    printf("register trace: first GPIOA write %08x (BRR of PA0/PA1), state at the jump %04x of 3fff, SP %08x\n",
           S->trace_first_gpio, S->trace_ok, S->sp_at_jump);
    EXPECT(o == OUT_JUMP && S->trace_first_gpio == 3u, "gates low before anything else on GPIOA\n");
    EXPECT(S->wd_reload_ns > 0 && S->wd_reload_ns < 2 * MS, "the IWDG reloaded with its new period at %.3f ms\n",
           S->wd_reload_ns / 1e6);
    app_ran(0);                       /* a warm boot with no mode: no loop, so no tick() before the jump */
    crash();
    o = boot_once();
    printf("register trace, warm boot without a mode: state at the jump %04x of 3fff, IWDG reloaded at %.3f ms\n",
           S->trace_ok, S->wd_reload_ns / 1e6);
    EXPECT(o == OUT_JUMP && S->requests == 0 && S->wd_reload_ns > 0 && S->wd_reload_ns < 2 * MS,
           "the reload with the new period comes from watchdog_start()\n");
    EXPECT(k_boot_id.magic == BOOT_ID_MAGIC && k_boot_id.version == 3 && k_boot_id.features == BOOT_FEATURES_V3 &&
           k_boot_id.reserved == 0xFFFFu && k_boot_id.app_max == 0x6800u && k_boot_id.mailbox == 0x20000BE0u,
           "boot_id\n");
    EXPECT(BOOT_FEATURES_V2 == 0x0Fu && BOOT_FEATURES_V3 == 0x1Fu, "features: tries, probe, button, link, hidden\n");
}

SCENARIO sc_lsi(void) {
    fresh();
    flash_set("n300");
    carry("n300");
    cold();
    boot_once();
    double normal = secs();
    fresh();
    flash_set("n300");
    carry("n300");
    S->lsi_dead = 1;
    cold();
    int o = boot_once();
    printf("LSI never ready: %s after %.3f s (normally %.3f s)\n", oname(o), secs(), normal);
    EXPECT(o == OUT_JUMP && secs() - normal < 0.1, "bounded waits\n");
    fresh();
    flash_set("n300");
    carry("n300");
    S->hsi_dead = 1;
    cold();
    o = boot_once();
    printf("HSIRDY never set: %s after %.3f s\n", oname(o), secs());
    EXPECT(o == OUT_JUMP && secs() - normal < 0.1, "bounded HSI wait\n");
}

/* ---- the real flash driver (REAL_FLASH_DRV) --------------------------------------------------- */

#ifdef REAL_FLASH_DRV
SCENARIO sc_drv(void) {
    fresh();                          /* stale flags at reset fail nothing (the v1 shim is gone) */
    flash_set("n300");
    carry("n301");
    S->drv_sr = 0x8011u;              /* OPTVERR | WRPERR | EOP */
    cold();
    app_ran(1);
    app_accepts();
    int o = boot_once();
    unsigned differ = 0;
    for (unsigned p = 0; p < (11628 + 127) / 128; p++) {
        uint8_t pg[128];
        memset(pg, 0xFF, 128);
        memcpy(pg, img("n301")->d + p * 128, p * 128 + 128 <= 11628 ? 128 : 11628 - p * 128);
        differ += memcmp(pg, img("n300")->d + p * 128, 128) != 0 || p * 128 + 128 > 11628;
    }
    printf("real driver, stale SR flags at reset: %s after %.2f s, %u erases, %u programs for %u differing pages, "
           "FLASH results %u\n", oname(o), secs(), S->drv_erases, S->drv_programs, differ, S->res[2]);
    EXPECT(o == OUT_RESET && S->res[0] == 1 && S->res[2] == 0 && S->drv_erases == S->drv_programs &&
           S->drv_erases <= differ && S->drv_erases + 2 >= differ && S->drv_locked_idle,
           "every page at the first try\n");
    o = boot_once();
    EXPECT(o == OUT_JUMP && flash_is("n301"), "new app\n");

    fresh();                          /* the first erase raises WRPERR, once: the second attempt */
    flash_set("n300");
    carry("n301");
    S->drv_wrperr = 2;
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    printf("real driver, the first erase raises WRPERR: %s after %.2f s, %u erases, %u programs, FLASH results %u\n",
           oname(o), secs(), S->drv_erases, S->drv_programs, S->res[2]);
    EXPECT(o == OUT_RESET && S->res[0] == 1 && S->res[2] == 0 && S->results_flash == 0 &&
           S->drv_erases == S->drv_programs + 1, "the page's second attempt, no FLASH result\n");
    S->drv_wrperr = 0;
    o = boot_once();
    EXPECT(o == OUT_JUMP && flash_is("n301"), "new app\n");

    fresh();                          /* BSY never clears */
    flash_set("n300");
    carry("n301");
    S->drv_bsy_stuck = 1;
    S->limit_ns = 120 * S_;
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    printf("real driver, BSY never clears: %s after %.1f s, FLASH results %u, largest watchdog gap %.1f ms\n", oname(o),
           secs(), S->res[2], S->iwdg_max_gap / 1e6);
    EXPECT(o == OUT_JUMP && S->results_flash == 3 && flash_is("n300") && S->iwdg_max_gap < 120 * MS,
           "FLASH reported, no watchdog reset, the untouched app after the ZTU's pause\n");

    fresh();                          /* every erase fails */
    flash_set("n300");
    carry("n301");
    S->drv_wrperr = 1;
    S->limit_ns = 60 * S_;
    cold();
    app_ran(1);
    app_accepts();
    o = boot_once();
    printf("real driver, every erase raises WRPERR: %s, FLASH results %u\n", oname(o), S->results_flash);
    EXPECT(S->results_flash == 3, "FLASH results\n");
}

/* The guard of the real driver, linked with boot.ld's range: called directly in a boot child. */
SCENARIO sc_drv_guard(void) {
    fresh();
    flash_set("n300");
    pid_t pid = fork();
    if (pid == 0) {
        child_init();
        static uint32_t page[32];
        memset(page, 0xA5, sizeof page);
        int r = 0;
        r |= flash_write_page(0x08000F80u, page) << 0;     /* sector 0 */
        r |= flash_write_page(0x08004800u, page) << 1;     /* the ring, above the app area */
        r |= flash_write_page(0x08004F80u, page) << 6;     /* the ring's last page */
        r |= flash_write_page(0x08008000u, page) << 7;     /* above the hidden flash */
        r |= flash_write_page(0x08001004u, page) << 2;     /* misaligned */
        r |= flash_write_page(0xFFFFFF80u, page) << 3;
        uint32_t e0, p0, u0, l0;
        void drv_model_stats(uint32_t *, uint32_t *, uint32_t *, uint32_t *);
        drv_model_stats(&e0, &p0, &u0, &l0);
        r |= (e0 != 0 || u0 != 0) << 4;
        r |= (flash_write_page(0x08004780u, page) != 1) << 5;   /* the last page of the area */
        r |= (flash_write_page(0x08005000u, page) != 1) << 8;   /* the first hidden page */
        r |= (flash_write_page(0x08007F80u, page) != 1) << 9;   /* the last hidden page */
        _exit(r);
    }
    int st;
    waitpid(pid, &st, 0);
    printf("real driver's write-range guard (boot.ld 0x08001000..0x08008000, not the ring): result %d\n",
           WEXITSTATUS(st));
    EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "guard\n");
    flash_set("n300");
}
#endif

/* ---- main ------------------------------------------------------------------------------------- */

typedef struct { const char *name; void (*fn)(void); int heavy; } scen_t;   /* heavy: started first */
static const scen_t k_scen[] = {
#ifdef REAL_FLASH_DRV
    /* the real driver: its own cases, and the paths that write without the flash model's knobs */
    { "drv", sc_drv, 0 },
    { "drv-guard", sc_drv_guard, 0 },
    { "asked", sc_asked, 0 },
    { "probe", sc_probe, 0 },
    { "crashloop", sc_crashloop, 0 },
    { "button", sc_button, 0 },
    { "link", sc_link, 0 },
    { "regs", sc_regs, 0 },
    { "hidden", sc_hidden, 0 },
#else
    { "asked", sc_asked, 0 },
    { "cuts-erase", sc_cuts_erase, 1 },
    { "cuts-program", sc_cuts_program, 1 },
    { "switch", sc_switch, 0 },
    { "stallp0", sc_stallp0, 0 },
    { "rescan", sc_rescan, 0 },
    { "noisy", sc_noisy, 1 },
    { "unsol", sc_unsol, 0 },
    { "flashfail", sc_flashfail, 0 },
    { "flashretry", sc_flashretry, 0 },
    { "badvec", sc_badvec, 0 },
    { "probe", sc_probe, 0 },
    { "probecoll", sc_probecoll, 1 },
    { "probedelay", sc_probedelay, 0 },
    { "probe-b3", sc_probe_b3, 0 },
    { "crashloop", sc_crashloop, 0 },
    { "wdg-carry", sc_wdg_carry, 0 },
    { "bigrescue", sc_bigrescue, 0 },
    { "benchrescue", sc_benchrescue, 0 },
    { "rescuecut", sc_rescuecut, 0 },
    { "giveup-noapp", sc_giveup_noapp, 0 },
    { "benchmodes", sc_benchmodes, 0 },
    { "limits", sc_limits, 0 },
    { "rescue-noapp", sc_rescue_noapp, 0 },
    { "duty", sc_duty, 0 },
    { "restored", sc_restored, 0 },
    { "button", sc_button, 0 },
    { "counter", sc_counter, 0 },
    { "link", sc_link, 0 },
    { "link-order", sc_link_order, 0 },
    { "ram", sc_ram, 0 },
    { "modes", sc_modes, 0 },
    { "request", sc_request_bytes, 0 },
    { "probe-refused", sc_probe_refused_after_writes, 0 },
    { "robust", sc_robust, 0 },
    { "stockztu", sc_stockztu, 1 },
    { "stockztu-garbage", sc_stockztu_garbage, 1 },
    { "fuzz1", sc_fuzz1, 1 },
    { "fuzz2", sc_fuzz2, 1 },
    { "fuzz3", sc_fuzz3, 1 },
    { "gates", sc_ztu_gates, 0 },
    { "swd", sc_swd, 0 },
    { "regs", sc_regs, 0 },
    { "lsi", sc_lsi, 0 },
    { "hidden", sc_hidden, 0 },
#endif
};

static void *map_at(uintptr_t a, size_t n, int shared) {
    void *p = mmap((void *)a, n, PROT_READ | PROT_WRITE,
                   (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)a) {
        fprintf(stderr, "cannot map 0x%08lx\n", (unsigned long)a);
        exit(2);
    }
    return p;
}

/* One scenario in a worker process of its own: its own chip-address mappings, so workers run in
 * parallel. Returns the number of failed checks. */
static int run_scenario(const scen_t *sc, uint64_t *worst) {
    FLASH_MEM = map_at(0x08000000u, 0x8000, 1);
    RAM_MEM   = map_at(0x20000000u, 0x1000, 1);
    TRIM_MEM  = map_at(0x1FFF0000u, 0x1000, 0);
    S = mmap(NULL, sizeof *S, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (S == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
    /* sector 0 and the settings above 14 KB hold patterns the bootloader must never change */
    for (int i = 0; i < 0x1000; i++) {
        boot_sector[i] = (uint8_t)(i * 7 + 3);
    }
    for (int i = 0; i < 0x800; i++) {
        settings_top[i] = (uint8_t)(i * 13 + 1);
    }
    memcpy(FLASH_MEM, boot_sector, sizeof boot_sector);
    memcpy(FLASH_MEM + 0x4800, settings_top, sizeof settings_top);
    memset(FLASH_MEM + (HIDDEN_BASE - BOOT_BASE), 0xFF, HIDDEN_SIZE);   /* the hidden flash: erased */
    /* factory words: HSI trim, flash timing for 24 MHz */
    uint32_t trim = 0x00001A5Bu, tw[5] = { 0x00AB12CDu, 0x07FF00EEu, 0x0001FFFFu, 0x00012345u, 0x3FFFABCDu };
    memcpy(TRIM_MEM + 0xF10, &trim, 4);
    memcpy(TRIM_MEM + 0xF6C, tw, sizeof tw);
    memcpy(TRIM_MEM + 0xF1C, tw, sizeof tw);
    g_scen = sc->name;
    sc->fn();
    *worst = S->worst_gap;
    return g_fail;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image dir> [scenario...]   (HARNESS_JOBS: parallel workers)\n", argv[0]);
        return 2;
    }
    g_dir = argv[1];
    enum { N = sizeof k_scen / sizeof k_scen[0] };
    int idx[N], n = 0;
    for (int i = 0; i < (int)N; i++) {
        int want = argc == 2;
        for (int a = 2; a < argc; a++) {
            want |= !strcmp(argv[a], k_scen[i].name);
        }
        if (want) {
            idx[n++] = i;
        }
    }
#ifdef REAL_FLASH_DRV
    const char *name = "test-boot (real flash driver)";
#else
    const char *name = "test-boot";
#endif
    if (!n) {
        printf("%s: no such scenario\n", name);
        return 2;
    }
    long jobs = getenv("HARNESS_JOBS") ? atol(getenv("HARNESS_JOBS")) : sysconf(_SC_NPROCESSORS_ONLN);
    jobs = jobs < 1 ? 1 : jobs;
    /* per worker: its output, its result */
    struct res { int fails, done; uint64_t worst; } *res =
        mmap(NULL, sizeof(struct res) * N, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    FILE *out[N];
    pid_t pid[N];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int started = 0, running = 0, printed = 0, fails = 0;
    uint64_t worst = 0;
    /* start the heavy ones first; print in the table's order */
    int order[N], m = 0;
    for (int pass = 1; pass >= 0; pass--) {
        for (int k = 0; k < n; k++) {
            if (!!k_scen[idx[k]].heavy == pass) {
                order[m++] = k;
            }
        }
    }
    int done[N];
    memset(done, 0, sizeof done);
    while (printed < n) {
        while (started < n && running < jobs) {
            int k   = order[started++];
            out[k]  = tmpfile();
            fflush(stdout);
            pid[k]  = fork();
            if (pid[k] == 0) {
                dup2(fileno(out[k]), 1);
                res[k].fails = run_scenario(&k_scen[idx[k]], &res[k].worst);
                res[k].done  = 1;
                fflush(stdout);
                _exit(0);
            }
            running++;
        }
        int   st;
        pid_t p = wait(&st);
        for (int j = 0; j < started; j++) {
            int k = order[j];
            if (pid[k] == p && !done[k]) {
                running--;
                done[k] = 1;
                if (!res[k].done) {   /* the worker itself died */
                    res[k].fails = 1;
                    fprintf(out[k], "  FAIL [%s] the scenario's worker died (status %d)\n", k_scen[idx[k]].name, st);
                }
            }
        }
        while (printed < n && done[printed]) {
            char buf[4096];
            size_t m;
            rewind(out[printed]);
            while ((m = fread(buf, 1, sizeof buf, out[printed])) > 0) {
                fwrite(buf, 1, m, stdout);
            }
            fclose(out[printed]);
            fails += res[printed].fails;
            worst  = res[printed].worst > worst ? res[printed].worst : worst;
            printed++;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("largest gap between watchdog refreshes over all boots: %.1f ms (limit %.0f ms)\n", worst / 1e6,
           GAP_LIMIT_NS / 1e6);
    printf("%s: %d scenarios, %.1f s with %ld workers: %s\n", name, n,
           (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, jobs, fails ? "FAILED" : "all passed");
    if (fails) {
        printf("%s: %d checks FAILED\n", name, fails);
    }
    return fails != 0;
}
