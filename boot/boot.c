/* Bootloader v2 of Open Dimmer, the ZDMS16-2 PY32 firmware: sector 0, 4 KB at 0x08000000.
 *
 * FROZEN once installed: it is replaced only over SWD (mains off), so it carries every escape
 * path that does not depend on the app, and leaves every policy that may still change (version
 * rules, offers, settings layout) to the app and the ZTU, which update over the air. The design
 * is docs/BOOTLOADER.md; "section n" below refers to it, and src/ota_proto.h holds every number
 * this file shares with the app and the ZTU.
 *
 * At every reset (section 4):
 *  - The gates go low first (Reset_Handler), the reset flags are copied before any RCC write,
 *    then the clock, the UART and the watchdog (~1 s, the period set here) start.
 *  - On a cold reset the module button, held when the power came and released within 5 s,
 *    asks for a rescue.
 *  - The mailbox says whether the app accepted an update, and carries the boot word: tries,
 *    probe answer time, info since the power-up, and the app's probe request (bit 7), which is
 *    read and cleared here at every boot.
 *  - One mode, in this order:
 *      update (the app asked, or no valid app)  takes the asked image, or any image
 *      rescue (the button, or 3 unconfirmed runs of a counted app)  any image but the installed
 *      probe  (a cold reset, or a warm one the app asked a probe for; never a bench build)
 *             takes a newer build only, and ends at the ZTU's first answer
 *    then the app starts.
 *  - Every mode first asks the ZTU for the header of the image it carries (0xA1, offset 0xC0,
 *    16 bytes) and pins the image only if it suits the mode and fits the installed settings
 *    layout. It then fetches the image from offset 0, checks page 0's vectors before the first
 *    write, writes page by page, checks the whole image, reports (0xA2) and restarts.
 * Nothing is erased before an image is pinned and its page 0 checked, so a ZTU that does not
 * answer leaves the installed app as it was. Once a page is written without a finished image
 * there is no valid app, and the bootloader asks until it gets one: no half app ever runs.
 *
 * The gate pins are driven low in pins_safe() and never touched again: the light is dark in
 * every bootloader state, and the app decides the light at its start. */
#include "flash_drv.h"
#include "ota_proto.h"
#include "py32f0xx.h"
#include <stddef.h>

/* No C library: small byte loops instead of its string functions. */
static void copy(uint8_t *d, const uint8_t *s, uint32_t n) {
    while (n--) {
        *d++ = *s++;
    }
}

static void fill(uint8_t *d, uint8_t v, uint32_t n) {
    while (n--) {
        *d++ = v;
    }
}

/* PA0/PA1 triac gates (high = conducting), PA5 LED (active low), PA6 module button (active
 * low, pull-up): docs/stock-firmware-analysis.md, pin table. */
#define PIN_GATES   ((1u << 0) | (1u << 1))
#define PIN_LED     (1u << 5)
#define PIN_BUTTON  (1u << 6)

#define FRAME_HEAD  8u                                                  /* 55 AA 02 seq(2) cmd len(2) */
#define RX_MAX      (FRAME_HEAD + OTA_BLOCK_HEAD + OTA_BLOCK_MAX + 1u)  /* the largest answer taken */
/* Bounded register waits (HSI, LSI, IWDG update): ~25-40 ms, normally a few LSI cycles. With an
 * LSI that never starts the watchdog simply does not run: never a hang before the app. */
#define WAIT_LOOPS  100000u

#define HDR         ((const app_header_t *)(APP_BASE + APP_HEADER_OFFSET))   /* the installed header */

/* SWD bench path (tools/ota_swd_emu.py): bytes placed here are read before the UART. */
volatile uint8_t  g_inject[RX_MAX];
volatile uint16_t g_inject_len, g_inject_pos;

/* State for SWD. The first 28 bytes keep the v1 bootloader's layout (tools/ota_swd_emu.py). */
volatile struct {
    uint32_t expect;       /* 0x00 next image offset wanted */
    uint32_t size, crc;    /* 0x04 0x08 the pinned image (crc 0: asking for a header) */
    uint8_t  version;      /* 0x0C its version byte */
    uint8_t  result;       /* 0x0D last OTA_RESULT_*, 0xFF none */
    uint8_t  erased;       /* 0x0E a page write was started in this boot */
    uint8_t  app_valid;    /* 0x0F the installed app passed app_valid() at the start */
    uint32_t requests, blocks, pages;   /* 0x10 0x14 0x18 */
    uint16_t build;        /* 0x1C build of the pinned image */
    uint8_t  mode;         /* 0x1E OTA_MODE_* of this boot, 0 none */
} g_boot;

static uint32_t g_ms;       /* ms since reset: SysTick's COUNTFLAG polled, so a lower bound */
static uint32_t g_word;     /* the boot word being built (BOOT_*, src/ota_proto.h) */
static uint8_t  g_mode;     /* OTA_MODE_* that decides what is taken; a link probe runs as PROBE */
static uint8_t  g_intact;   /* the installed app is valid: checked, and nothing written since */
static uint32_t g_limit;    /* largest image the installed settings layout leaves room for */
static uint32_t g_expect;   /* OTA_MODE_ASKED: the only CRC taken (the app accepted it) */
static uint32_t g_skip;     /* rescue: the installed CRC, never taken again (it would rewrite nothing) */
static uint32_t g_bad;      /* refused at page 0 in this run */
static uint32_t g_asked;    /* g_ms of the last request: the pace goes on over a give-up */
static uint8_t  g_req[OTA_REQ_LEN];   /* 0xA1 request v2; bytes 6..15 set at the start (mode: NO_APP after a give-up) */
static uint8_t  g_rx[RX_MAX];
static uint16_t g_rx_len;
static uint32_t g_page[FLASH_PAGE_BYTES / 4u];
static uint16_t g_fill;

/* The bootloader's identity, at 0x08000010 (boot.ld): the app reads it for its image limit,
 * identity byte 9 and the link watchdog. */
__attribute__((section(".boot_id"), used))
const boot_id_t k_boot_id = { BOOT_ID_MAGIC, BOOT_VERSION, BOOT_FEATURES_V2, 0xFFFFu,
                              BOOT_APP_MAX, OTA_MAILBOX_RAM };

/* ---- hardware -------------------------------------------------------------------------------- */

/* Plain register writes: the LL GPIO helpers find the pin position with a software bit scan on
 * the M0+, which costs bootloader space for nothing. ODR is low after reset; BRR before MODER
 * keeps it so while the gate pins become outputs. */
static void pins_safe(void) {
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    GPIOA->BRR   = PIN_GATES;                                        /* gates low */
    GPIOA->BSRR  = PIN_LED;                                          /* LED off */
    GPIOA->MODER = (GPIOA->MODER & ~((3u << 0) | (3u << 2) | (3u << 10) | (3u << 12))) |
                   (1u << 0) | (1u << 2) | (1u << 10);               /* PA0 PA1 PA5 out, PA6 in */
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 12)) | (1u << 12);        /* PA6 pull-up */
}

static void clock_and_uart(void) {
    RCC->ICSCR = (RCC->ICSCR & ~(RCC_ICSCR_HSI_FS | RCC_ICSCR_HSI_TRIM)) |
                 (4u << 13) | (*(const uint32_t *)0x1FFF0F10 & 0x1FFFu);   /* HSI 24 MHz, factory trim */
    for (uint32_t n = WAIT_LOOPS; !(RCC->CR & RCC_CR_HSIRDY) && --n;) {
    }
    SysTick->LOAD = 24000u - 1u;                                     /* 1 ms, polled */
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

    RCC->APBENR2  |= RCC_APBENR2_USART1EN;
    GPIOA->AFR[0]  = (GPIOA->AFR[0] & ~((0xFu << 8) | (0xFu << 12))) | (1u << 8) | (1u << 12);
    GPIOA->PUPDR   = (GPIOA->PUPDR & ~(3u << 6)) | (1u << 6);        /* RX pull-up */
    GPIOA->MODER   = (GPIOA->MODER & ~((3u << 4) | (3u << 6))) | (2u << 4) | (2u << 6);
    USART1->BRR    = 208u;                                           /* 24 MHz / 115200 */
    USART1->CR1    = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

/* The watchdog with this bootloader's own period at every boot (~1 s: LSI / 32, reload 1000, as
 * the app sets it), so neither the reset values nor an app's choice matter, and whether a
 * software reset stops a running IWDG does not either. A new RLR reaches the counter only at a
 * reload, and 0xCCCC does not reload a running IWDG: the reload after the update wait ends a
 * short countdown that a previous app left running, before app_valid() and before the boot
 * counts. Option byte IWDG_SW stays 1. */
static void watchdog_start(void) {
    RCC->CSR |= RCC_CSR_LSION;
    for (uint32_t n = WAIT_LOOPS; !(RCC->CSR & RCC_CSR_LSIRDY) && --n;) {
    }
    IWDG->KR  = 0xCCCC;                                              /* start */
    IWDG->KR  = 0x5555;                                              /* PR and RLR writable */
    IWDG->PR  = 3;
    IWDG->RLR = 1000;
    for (uint32_t n = WAIT_LOOPS; IWDG->SR && --n;) {
    }
    IWDG->KR = 0xAAAA;                                               /* reload: the new period */
}

/* Every pass of every loop: the watchdog, and the ms count. */
static void tick(void) {
    IWDG->KR = 0xAAAA;
    if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) {
        g_ms++;
    }
}

static void led(uint8_t on) {
    if (on) {
        GPIOA->BRR = PIN_LED;
    } else {
        GPIOA->BSRR = PIN_LED;
    }
}

/* 1 if PA6 (the module button) reads low on every pass for ms. */
static uint8_t held(uint32_t ms) {
    for (uint32_t t = g_ms; g_ms - t < ms;) {
        tick();
        if (GPIOA->IDR & PIN_BUTTON) {
            return 0;
        }
    }
    return 1;
}

static int rx_byte(void) {
    if (g_inject_pos < g_inject_len) {
        return g_inject[g_inject_pos++];
    }
    if (USART1->SR & (USART_SR_RXNE | USART_SR_ORE)) {
        return (uint8_t)USART1->DR;                                  /* reading DR also clears ORE */
    }
    return -1;
}

static void tx_byte(uint8_t b) {
    while (!(USART1->SR & USART_SR_TXE)) {
    }
    USART1->DR = b;
}

/* One frame to the ZTU (~2.2 ms for a request). RX is not read meanwhile: the one-byte data
 * register overruns, which is why run() never starts a request into an arriving frame. */
static void send(uint8_t cmd, const uint8_t *d, uint16_t n) {
    uint8_t h[FRAME_HEAD] = { 0x55, 0xAA, 0x02, 0, 0, cmd, (uint8_t)(n >> 8), (uint8_t)n };
    uint8_t s = 0;
    for (uint8_t i = 0; i < FRAME_HEAD; i++) {
        s += h[i];
        tx_byte(h[i]);
    }
    for (uint16_t i = 0; i < n; i++) {
        s += d[i];
        tx_byte(d[i]);
    }
    tx_byte(s);
    while (!(USART1->SR & USART_SR_TC)) {
    }
}

static void rx_drop(uint16_t n) {
    g_rx_len = (uint16_t)(g_rx_len - n);
    copy(g_rx, g_rx + n, g_rx_len);
}

/* Collects ZTU bytes. Returns the length of a complete frame with a good checksum at g_rx[0],
 * which the caller drops when done, or 0. A byte that cannot start a frame is dropped at once,
 * so noise never holds back a request (run()'s half-duplex rule). After a bad checksum or a
 * length no answer of ours has, the buffered bytes are scanned again from the next byte: a real
 * frame may start inside. Half a frame that stops coming is dropped after FRAME_SILENCE_MS. A
 * byte is read only while the buffer holds no complete frame, so it never overflows. */
static uint16_t rx_frame(void) {
    static uint32_t last;
    int             b;

    if (g_rx_len && g_ms - last > FRAME_SILENCE_MS) {
        g_rx_len = 0;
    }
    for (;;) {
        while (g_rx_len && (g_rx[0] != 0x55 || (g_rx_len > 1 && g_rx[1] != 0xAA))) {
            rx_drop(1);
        }
        if (g_rx_len >= FRAME_HEAD) {
            uint32_t whole = FRAME_HEAD + ((uint32_t)g_rx[6] << 8 | g_rx[7]) + 1u;
            if (whole > RX_MAX) {
                rx_drop(1);
                continue;
            }
            if (g_rx_len >= whole) {
                uint8_t s = 0;
                for (uint16_t i = 0; i < whole - 1u; i++) {
                    s += g_rx[i];
                }
                if (s == g_rx[whole - 1u]) {
                    return (uint16_t)whole;
                }
                rx_drop(1);
                continue;
            }
        }
        if ((b = rx_byte()) < 0) {
            return 0;
        }
        last              = g_ms;
        g_rx[g_rx_len++] = (uint8_t)b;
    }
}

/* ---- image ----------------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The build number of a header, as the bytes come (0: none, 2.1.x and older). */
static uint32_t build_of(const uint8_t *h) {
    return (uint32_t)h[offsetof(app_header_t, build)] | ((uint32_t)h[offsetof(app_header_t, build) + 1u] << 8);
}

/* app_valid()'s vector rule, also applied to a fetched page 0 before anything is written: the
 * initial SP in RAM, the reset vector inside the image. Stricter checks (SP exactly the top of
 * RAM, an odd reset vector) stay in the tools and the ZTU, which change over the air. */
static uint8_t vec_ok(const uint32_t *v, uint32_t size) {
    return (v[0] >> 20) == 0x200u && v[1] >= APP_BASE && v[1] < APP_BASE + size;
}

/* The installed app: magic, 0xD0 <= size <= BOOT_APP_MAX, vectors, CRC (~43 ms for 11.6 KB). */
static uint8_t app_valid(void) {
    const app_header_t *h = HDR;

    if (h->magic != APP_MAGIC || h->size < APP_SIZE_MIN || h->size > BOOT_APP_MAX ||
        !vec_ok((const uint32_t *)APP_BASE, h->size)) {
        return 0;
    }
    const uint8_t *img  = (const uint8_t *)APP_BASE;
    uint32_t       at   = APP_HEADER_OFFSET + offsetof(app_header_t, crc);
    const uint8_t  z[4] = { 0, 0, 0, 0 };
    uint32_t       crc  = crc32_update(0, img, at);
    crc = crc32_update(crc, z, 4);
    crc = crc32_update(crc, img + at + 4, h->size - at - 4);
    return crc == h->crc;
}

/* A counted app: one that confirms its runs, and no bench build (section 7). */
static uint8_t counted(void) {
    return (HDR->flags & (APP_FLAG_CONFIRMS | APP_FLAGS_BENCH)) == APP_FLAG_CONFIRMS;
}

static void word_store(void) {
    OTA_MAILBOX->boot       = g_word;
    OTA_MAILBOX->boot_check = ~g_word;
}

static void jump_app(void) {
    led(0);
    __disable_irq();
    SysTick->CTRL = 0;
    RCC->APBRSTR2 |= RCC_APBRSTR2_USART1RST;                         /* back to its reset state */
    RCC->APBRSTR2 &= ~RCC_APBRSTR2_USART1RST;
    uint32_t sp = ((const uint32_t *)APP_BASE)[0];
    uint32_t pc = ((const uint32_t *)APP_BASE)[1];
    SCB->VTOR   = APP_BASE;
    __set_MSP(sp);
    __enable_irq();
    ((void (*)(void))(uintptr_t)pc)();
}

/* ---- update protocol (section 8) ------------------------------------------------------------- */

static void ask(uint32_t off, uint16_t want) {
    put_be32(g_req + OTA_REQ_AT_OFFSET, off);
    g_req[OTA_REQ_AT_MAX_LEN]      = (uint8_t)(want >> 8);
    g_req[OTA_REQ_AT_MAX_LEN + 1u] = (uint8_t)want;
    send(OTA_CMD_BLOCK, g_req, sizeof(g_req));
    g_boot.requests++;
}

/* The next block of the pinned image: never across a page. */
static void ask_block(void) {
    uint32_t left = g_boot.size - g_boot.expect;
    uint32_t room = FLASH_PAGE_BYTES - g_fill;
    ask(g_boot.expect, (uint16_t)(left < room ? left : room));
}

static void result(uint8_t status) {
    uint8_t d[OTA_RESULT_LEN] = { status, g_boot.version, (uint8_t)(g_boot.build >> 8), (uint8_t)g_boot.build };
    g_boot.result = status;
    send(OTA_CMD_RESULT, d, sizeof(d));
}

/* An answer to a header request (offset 0xC0, the 16 header bytes): the image is pinned when it
 * suits the mode. All modes: "APDZ", the header agreeing with the block header, no bench flag,
 * 0xD0 <= size <= the installed layout's limit, not the installed CRC in a rescue, not refused
 * at page 0 in this run, CRC not 0 (0 means "none" in g_skip and g_bad). Asked: the CRC the app
 * accepted. Probe: a build larger than the one installed at this boot (a header without one
 * counts as 0; taken from the request, which holds the installed header as it was before any
 * write), within the normal build range. Rescue and no app: any other image, older ones
 * included. */
enum { PICK_NONE, PICK_REFUSE, PICK_TAKE };

static uint8_t pick(const uint8_t *d, uint16_t n) {
    if (n < OTA_HEADER_ANSWER_LEN || be32(d + OTA_BLOCK_AT_OFFSET) != APP_HEADER_OFFSET) {
        return PICK_NONE;
    }
    const uint8_t *h     = d + OTA_BLOCK_AT_DATA;
    uint32_t       size  = be32(d + OTA_BLOCK_AT_SIZE);
    uint32_t       crc   = be32(d + OTA_BLOCK_AT_CRC);
    uint32_t       build = build_of(h);
    /* the installed header's build, as the request carries its bytes 8..15 */
    uint32_t       had   = build_of(g_req + OTA_REQ_AT_INSTALLED - offsetof(app_header_t, crc));

    if (le32(h) != APP_MAGIC || le32(h + offsetof(app_header_t, size)) != size ||
        le32(h + offsetof(app_header_t, crc)) != crc || size < APP_SIZE_MIN || size > g_limit ||
        (h[offsetof(app_header_t, flags)] & APP_FLAGS_BENCH) || crc == 0 || crc == g_skip ||
        crc == g_bad || (g_mode == OTA_MODE_ASKED && crc != g_expect) ||
        (g_mode == OTA_MODE_PROBE && (build <= had || build > OTA_BUILD_MAX))) {
        return PICK_REFUSE;
    }
    g_boot.size    = size;
    g_boot.crc     = crc;
    g_boot.version = h[offsetof(app_header_t, version)];
    g_boot.build   = (uint16_t)build;
    g_boot.expect  = 0;
    g_fill         = 0;
    return PICK_TAKE;
}

/* A block while an image is pinned. TOOK: accepted (ask for the next one at once). STRAY: of the
 * pinned image, but not the block asked for (late, duplicate, too long): asked again after
 * REJECT_PACE_MS. OTHER: not the pinned image (stays at the normal pace). DONE: written and
 * valid, reported; the caller restarts. A failed page or image drops the pin (g_boot.crc = 0):
 * the header is asked for again. */
enum { BLOCK_OTHER, BLOCK_STRAY, BLOCK_TOOK, BLOCK_DONE };

static uint8_t on_block(const uint8_t *d, uint16_t n) {
    if (n < OTA_BLOCK_HEAD) {
        return BLOCK_STRAY;
    }
    uint32_t off  = be32(d + OTA_BLOCK_AT_OFFSET);
    uint32_t size = be32(d + OTA_BLOCK_AT_SIZE);
    uint32_t crc  = be32(d + OTA_BLOCK_AT_CRC);
    uint16_t len  = (uint16_t)(n - OTA_BLOCK_HEAD);

    if (crc != g_boot.crc || size != g_boot.size) {
        return BLOCK_OTHER;
    }
    if (len == 0 || off != g_boot.expect || len > FLASH_PAGE_BYTES - g_fill || len > size - off) {
        return BLOCK_STRAY;            /* (off == expect < size here, so size - off cannot wrap) */
    }
    copy((uint8_t *)g_page + g_fill, d + OTA_BLOCK_AT_DATA, len);
    g_fill        = (uint16_t)(g_fill + len);
    g_boot.expect += len;
    g_boot.blocks++;

    if (g_fill == FLASH_PAGE_BYTES || g_boot.expect == size) {
        fill((uint8_t *)g_page + g_fill, 0xFF, FLASH_PAGE_BYTES - g_fill);
        uint32_t addr = APP_BASE + g_boot.expect - g_fill;
        g_fill        = 0;
        if (addr == APP_BASE && !vec_ok(g_page, size)) {
            result(OTA_RESULT_BAD_IMAGE);                            /* nothing written: refused */
            g_bad      = crc;
            g_boot.crc = 0;
            return BLOCK_OTHER;
        }
        g_intact      = 0;
        g_boot.erased = 1;
        if (!flash_write_page(addr, g_page) && !flash_write_page(addr, g_page)) {
            result(OTA_RESULT_FLASH);
            g_boot.crc = 0;
            return BLOCK_TOOK;
        }
        g_boot.pages++;
    }
    if (g_boot.expect < size) {
        return BLOCK_TOOK;
    }
    if (app_valid() && HDR->crc == crc) {
        result(OTA_RESULT_OK);
        return BLOCK_DONE;
    }
    result(OTA_RESULT_BAD_IMAGE);
    g_boot.crc = 0;
    return BLOCK_TOOK;
}

/* The loop of every mode. Without a pinned image it asks for the carried header (every
 * PROBE_EVERY_MS in the probe, else REQUEST_EVERY_MS) and pins an image that suits the mode; the
 * probe ends at its first header answer that does not suit. A pinned image is fetched block by
 * block and written; a finished one ends in a reset. Returns once `window` passed without
 * progress (GIVE_UP_MS after a taken header or block): the caller starts the app if it is
 * intact. A request goes out only while no frame is half received, because send() does not read
 * RX and a request sent into an arriving answer destroys it (section 8, half-duplex). */
static void run(uint32_t window) {
    uint32_t t0 = g_ms, progress = t0;
    uint8_t  writing = 0, answered = 0;

    for (;;) {
        tick();
        /* LED: off in the probe until a block is taken; slow blink while listening, fast once
         * a block was taken (section 4). */
        led((g_mode != OTA_MODE_PROBE || writing) && ((g_ms >> (writing ? 7 : 9)) & 1u));
        if (g_ms - progress > window) {
            return;
        }
        if (!g_rx_len &&
            g_ms - g_asked >= (g_mode == OTA_MODE_PROBE && !g_boot.crc ? PROBE_EVERY_MS : REQUEST_EVERY_MS)) {
            g_asked = g_ms;
            if (g_boot.crc) {
                ask_block();
            } else {
                ask(APP_HEADER_OFFSET, sizeof(app_header_t));
            }
        }
        uint16_t len = rx_frame();
        if (!len) {
            continue;
        }
        uint16_t       n = (uint16_t)((g_rx[6] << 8) | g_rx[7]);
        const uint8_t *d = g_rx + FRAME_HEAD;
        uint8_t        r = BLOCK_OTHER;

        if (g_rx[5] != OTA_CMD_BLOCK) {
            /* Not ours (DP traffic, queries of a stock ZTU, ...): ignored. */
        } else if (!g_boot.crc) {
            uint8_t p = pick(d, n);
            if (p != PICK_NONE && g_mode == OTA_MODE_PROBE && !answered) {
                answered  = 1;                                       /* first answer: < 4096 ms */
                g_word   |= ((g_ms - t0) << BOOT_PROBE_MS_SHIFT) & BOOT_PROBE_MS_MASK;
            }
            if (p == PICK_TAKE) {
                progress = g_ms;
                window   = GIVE_UP_MS;
                g_asked  = g_ms - REQUEST_EVERY_MS;                  /* ask for offset 0 now */
            } else if (p == PICK_REFUSE && g_mode == OTA_MODE_PROBE) {
                return;                                              /* nothing newer: the app */
            }
        } else {
            r = on_block(d, n);
        }
        rx_drop(len);
        if (r == BLOCK_DONE) {
            g_word = (g_word & ~BOOT_TRIES_MASK) | BOOT_INFO_WRITTEN;   /* a new image: 3 fresh runs */
            word_store();
            for (uint32_t t = g_ms; g_ms - t < RESULT_FLUSH_MS;) {   /* let the result frame leave */
                tick();
            }
            NVIC_SystemReset();
        }
        if (r == BLOCK_TOOK) {
            writing  = 1;
            progress = g_ms;
            window   = GIVE_UP_MS;
            g_asked  = g_ms - REQUEST_EVERY_MS;
        } else if (r == BLOCK_STRAY) {
            g_asked = g_ms - REQUEST_EVERY_MS + REJECT_PACE_MS;
        }
    }
}

/* ---- boot flow (section 4) ------------------------------------------------------------------- */

int main(void);
int main(void) {
    uint32_t csr = RCC->CSR;           /* first: watchdog_start()'s LSION write is a read-modify-
                                          write of this register; the bootloader never sets RMVF */
    clock_and_uart();
    watchdog_start();
    g_boot.result = 0xFF;

    /* Cold: power-up, brown-out, NRST, option reload. The bootloader's own reset after a probe
     * or button update leaves PWRRSTF set next to SFTRSTF (no app cleared the flags): warm. */
    uint8_t cold = !(csr & (RCC_CSR_SFTRSTF | RCC_CSR_IWDGRSTF));
    uint8_t button = 0, stuck = 0;
    if (cold && held(BUTTON_MS)) {     /* pressed before the power came */
        led(1);                        /* let go now: the window opens at the release, so no
                                          app ever sees the rescue press */
        stuck  = held(BUTTON_RELEASE_MS);
        button = !stuck;
        led(0);
    }

    volatile ota_mailbox_t *mb = OTA_MAILBOX;
    uint8_t asked = mb->magic == OTA_MAILBOX_MAGIC && mb->magic_inv == ~OTA_MAILBOX_MAGIC;
    g_expect      = mb->expect_crc;
    mb->magic     = 0;
    mb->magic_inv = 0;

    /* The boot word: kept over a warm reset when valid, else rebuilt (a power-up also drops a
     * probe request from before it). Bit 7, the app's probe request, is read and cleared here
     * at every boot, honoured or not, and the cleared word is stored before anything else can
     * reset: a request never outlives the boot that read it. */
    g_word = mb->boot;
    if (cold || !BOOT_WORD_VALID(g_word, mb->boot_check)) {
        g_word = BOOT_WORD_FRESH;
    }
    uint8_t link = (g_word & BOOT_PROBE_REQ) != 0;
    g_word &= ~BOOT_PROBE_REQ;
    if (stuck) {
        g_word |= BOOT_INFO_STUCK;
    }

    const app_header_t *h = HDR;
    g_intact         = app_valid();
    g_boot.app_valid = g_intact;
    g_limit          = (h->flags & APP_FLAG_RING_HIGH) ? BOOT_APP_MAX : APP_LIMIT_LEGACY;
    g_req[OTA_REQ_AT_BOOT] = BOOT_VERSION;
    copy(g_req + OTA_REQ_AT_INSTALLED, (const uint8_t *)h + offsetof(app_header_t, crc), 8);

    uint32_t window = GIVE_UP_MS;
    if (!g_intact || asked) {
        g_word &= ~BOOT_TRIES_MASK;    /* the app proved its update path, or there is none */
        if (g_intact) {
            g_mode = OTA_MODE_ASKED;
        } else {
            g_mode  = OTA_MODE_NO_APP;
            g_limit = BOOT_APP_MAX;    /* layout unknown: recovery comes before settings */
        }
    } else if (button || (counted() && (g_word & BOOT_TRIES_MASK) >= BOOT_TRIES_MAX)) {
        g_skip = h->crc;
        window = RESCUE_WINDOW_MS;
        if (button) {
            g_word |= BOOT_INFO_BUTTON;
            g_mode  = OTA_MODE_BUTTON;
        } else {
            g_word      |= BOOT_INFO_RESCUE;
            mb->restored = OTA_RESTORED_MAGIC;   /* a crash loop: the next start takes the
                                                    power-on behaviour, never a stale restore */
            g_mode       = OTA_MODE_RESCUE;
        }
    } else if ((cold || link) && !(h->flags & APP_FLAGS_BENCH)) {
        g_mode = OTA_MODE_PROBE;
        window = PROBE_WINDOW_MS;
        if (!cold) {                   /* asked by the app's link watchdog: a link probe */
            g_word = (g_word & ~BOOT_PROBE_MS_MASK) | BOOT_INFO_LINK;
        }
    }
    g_req[OTA_REQ_AT_MODE] = (g_mode == OTA_MODE_PROBE && !cold) ? OTA_MODE_LINK : g_mode;
    word_store();

    g_asked = g_ms - REQUEST_EVERY_MS;   /* the first request at once */
    while (g_mode) {
        g_boot.mode = g_req[OTA_REQ_AT_MODE];
        run(window);
        if (g_intact || (g_intact = app_valid()) != 0) {
            break;
        }
        /* No valid app: any image beats none. Unpin, unskip, unrefuse, the full ceiling, and
         * 10 s windows from now on. */
        g_mode = g_req[OTA_REQ_AT_MODE] = OTA_MODE_NO_APP;
        g_expect = g_skip = g_bad = g_boot.crc = 0;
        g_limit  = BOOT_APP_MAX;
        window   = GIVE_UP_MS;
    }
    if (counted() && (g_word & BOOT_TRIES_MASK) < BOOT_TRIES_MAX) {
        g_word++;                      /* one more unconfirmed run, until the app confirms */
    }
    word_store();
    jump_app();
    for (;;) {
    }
}

/* ---- startup --------------------------------------------------------------------------------- */

/* Own minimal startup: no interrupts are used, so the vector table only needs the stack, reset
 * and the fault entries (a fault restarts and tries again). */
extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;

void Reset_Handler(void) {
    pins_safe();                       /* first: the gate pins float from reset until here */
    const uint32_t *s = &_sidata;
    uint32_t       *d = &_sdata;
    while (d < &_edata) {
        *d++ = *s++;
    }
    for (d = &_sbss; d < &_ebss;) {
        *d++ = 0;
    }
    main();
    for (;;) {
    }
}

static void fault(void) {
    NVIC_SystemReset();
}

__attribute__((section(".isr_vector"), used))
static const void *const k_vectors[4] = { &_estack, (void *)Reset_Handler, (void *)fault, (void *)fault };
