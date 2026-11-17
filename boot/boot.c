/* Bootloader of the ZDMS16-2 PY32 firmware (2 KB at 0x08000000).
 *
 * At reset: gates low first. If the app image is valid and nobody asked for an
 * update, start it. Otherwise ask the Zigbee module for the image block by
 * block (ota_proto.h), write it, check it, report, and restart.
 * - Nothing is erased before the first block has arrived: an update that the
 *   module does not answer (10 s) falls back to the old app.
 * - Once erasing has begun, or without a valid app, it keeps asking forever:
 *   a power cut half way leaves an app that fails its CRC, and the next boot
 *   asks again. No way to end up with a half app running. */
#include "flash_drv.h"
#include "ota_proto.h"
#include "py32f0xx.h"
#include <stddef.h>

/* No C library (2 KB): small byte loops instead of its string functions. */
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


#define CHUNK_MAX         128u
#define FRAME_HEADER      8u
#define BLOCK_HEAD        13u                      /* offset size crc version */
#define RX_MAX            (FRAME_HEADER + BLOCK_HEAD + CHUNK_MAX + 1u)
#define REQUEST_EVERY_MS  500u
#define GIVE_UP_MS        10000u

/* Bench test over SWD: bytes placed here are read before the UART. */
volatile uint8_t  g_inject[RX_MAX];
volatile uint16_t g_inject_len, g_inject_pos;

/* State for SWD. */
volatile struct {
    uint32_t expect;       /* next offset wanted */
    uint32_t size, crc;    /* image being received */
    uint8_t  version;
    uint8_t  result;       /* last OTA_RESULT_*, 0xFF none */
    uint8_t  erased;       /* app area touched */
    uint8_t  app_valid;
    uint32_t requests, blocks, pages;
} g_boot;

static uint32_t g_ms;
static uint8_t  g_rx[RX_MAX];
static uint16_t g_rx_len;
static uint32_t g_page[FLASH_PAGE_BYTES / 4u];
static uint16_t g_fill;

/* ---- hardware ------------------------------------------------------------- */

/* Plain register writes: the LL GPIO helpers find the pin position with a
 * software bit scan on the M0+, which costs too much of the 2 KB. */
static void pins_safe(void) {
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    GPIOA->BRR   = (1u << 0) | (1u << 1);                    /* gates low */
    GPIOA->BSRR  = 1u << 5;                                  /* LED off (active low) */
    GPIOA->MODER = (GPIOA->MODER & ~((3u << 0) | (3u << 2) | (3u << 10))) |
                   (1u << 0) | (1u << 2) | (1u << 10);       /* PA0 PA1 PA5 outputs */
}

static void clock_and_uart(void) {
    RCC->ICSCR = (RCC->ICSCR & ~(RCC_ICSCR_HSI_FS | RCC_ICSCR_HSI_TRIM)) |
                 (4u << 13) | (*(const uint32_t *)0x1FFF0F10 & 0x1FFFu);   /* HSI 24 MHz, factory trim */
    while (!(RCC->CR & RCC_CR_HSIRDY)) {
    }
    SysTick->LOAD = 24000u - 1u;                             /* 1 ms, polled */
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

    RCC->APBENR2  |= RCC_APBENR2_USART1EN;
    GPIOA->AFR[0]  = (GPIOA->AFR[0] & ~((0xFu << 8) | (0xFu << 12))) | (1u << 8) | (1u << 12);
    GPIOA->PUPDR   = (GPIOA->PUPDR & ~(3u << 6)) | (1u << 6);   /* RX pull-up */
    GPIOA->MODER   = (GPIOA->MODER & ~((3u << 4) | (3u << 6))) | (2u << 4) | (2u << 6);
    USART1->BRR    = 208u;                                   /* 24 MHz / 115200 */
    USART1->CR1    = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

static void tick(void) {
    if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) {
        g_ms++;
    }
}

static int rx_byte(void) {
    if (g_inject_pos < g_inject_len) {
        return g_inject[g_inject_pos++];
    }
    uint32_t sr = USART1->SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        return (uint8_t)USART1->DR;                          /* reading DR also clears ORE */
    }
    return -1;
}

static void tx_byte(uint8_t b) {
    while (!(USART1->SR & USART_SR_TXE)) {
    }
    USART1->DR = b;
}

static void send(uint8_t cmd, const uint8_t *d, uint16_t n) {
    uint8_t h[FRAME_HEADER] = { 0x55, 0xAA, 0x02, 0, 0, cmd, (uint8_t)(n >> 8), (uint8_t)n };
    uint8_t s = 0;
    for (uint8_t i = 0; i < FRAME_HEADER; i++) {
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

/* Collect bytes; returns 1 with a complete, checked frame in g_rx. */
static uint8_t rx_frame(void) {
    int b;
    while ((b = rx_byte()) >= 0) {
        if (g_rx_len < RX_MAX) {
            g_rx[g_rx_len++] = (uint8_t)b;
        } else {
            g_rx_len = 0;
        }
        while (g_rx_len >= 2 && (g_rx[0] != 0x55 || g_rx[1] != 0xAA)) {
            copy(g_rx, g_rx + 1, --g_rx_len);
        }
        if (g_rx_len < FRAME_HEADER) {
            continue;
        }
        uint16_t n = (uint16_t)((g_rx[6] << 8) | g_rx[7]);
        if (FRAME_HEADER + n + 1u > RX_MAX) {
            g_rx_len = 0;                                     /* not ours: resync */
            continue;
        }
        if (g_rx_len < FRAME_HEADER + n + 1u) {
            continue;
        }
        uint8_t s = 0;
        for (uint16_t i = 0; i < FRAME_HEADER + n; i++) {
            s += g_rx[i];
        }
        uint8_t ok = s == g_rx[FRAME_HEADER + n];
        if (ok) {
            return 1;                                         /* caller resets g_rx_len */
        }
        g_rx_len = 0;
    }
    return 0;
}

/* ---- image ---------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint8_t app_valid(void) {
    const app_header_t *h = (const app_header_t *)(APP_BASE + APP_HEADER_OFFSET);
    const uint32_t     *v = (const uint32_t *)APP_BASE;

    if (h->magic != APP_MAGIC || h->size < APP_HEADER_OFFSET + sizeof(*h) ||
        h->size > APP_MAX_SIZE || (v[0] >> 20) != 0x200 ||
        v[1] < APP_BASE || v[1] >= APP_BASE + h->size) {
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

static void jump_app(void) {
    __disable_irq();
    SysTick->CTRL = 0;
    RCC->APBRSTR2 |= RCC_APBRSTR2_USART1RST;                  /* back to reset state for the app */
    RCC->APBRSTR2 &= ~RCC_APBRSTR2_USART1RST;
    uint32_t sp = ((const uint32_t *)APP_BASE)[0];
    uint32_t pc = ((const uint32_t *)APP_BASE)[1];
    SCB->VTOR   = APP_BASE;
    __set_MSP(sp);
    __enable_irq();
    ((void (*)(void))pc)();
}

static void request(void) {
    uint32_t left = g_boot.size ? g_boot.size - g_boot.expect : CHUNK_MAX;
    uint32_t room = FLASH_PAGE_BYTES - g_fill;
    uint16_t want = (uint16_t)(left < room ? left : room);
    uint8_t  d[6] = { (uint8_t)(g_boot.expect >> 24), (uint8_t)(g_boot.expect >> 16),
                      (uint8_t)(g_boot.expect >> 8), (uint8_t)g_boot.expect,
                      (uint8_t)(want >> 8), (uint8_t)want };
    send(OTA_CMD_BLOCK, d, sizeof(d));
    g_boot.requests++;
}

static void restart_transfer(void) {
    g_boot.expect = 0;
    g_boot.size   = 0;
    g_fill        = 0;
}

static void result(uint8_t status) {
    uint8_t d[2] = { status, g_boot.version };
    g_boot.result = status;
    send(OTA_CMD_RESULT, d, sizeof(d));
}

/* A block from the module. Returns 1 when the image is complete and good. */
static uint8_t on_block(const uint8_t *d, uint16_t n) {
    if (n < BLOCK_HEAD) {
        return 0;
    }
    uint32_t off = be32(d), size = be32(d + 4), crc = be32(d + 8);
    uint16_t len = (uint16_t)(n - BLOCK_HEAD);

    if (size < APP_HEADER_OFFSET + sizeof(app_header_t) || size > APP_MAX_SIZE || len == 0) {
        return 0;
    }
    if (g_boot.size && (size != g_boot.size || crc != g_boot.crc)) {
        restart_transfer();                                   /* the module has another image now */
    }
    if (off != g_boot.expect || len > FLASH_PAGE_BYTES - g_fill || off + len > size) {
        return 0;                                             /* not what we asked for */
    }
    g_boot.size    = size;
    g_boot.crc     = crc;
    g_boot.version = d[12];
    copy((uint8_t *)g_page + g_fill, d + BLOCK_HEAD, len);
    g_fill        = (uint16_t)(g_fill + len);
    g_boot.expect += len;
    g_boot.blocks++;

    if (g_fill == FLASH_PAGE_BYTES || g_boot.expect == size) {
        fill((uint8_t *)g_page + g_fill, 0xFF, FLASH_PAGE_BYTES - g_fill);
        uint32_t addr  = APP_BASE + g_boot.expect - g_fill;
        g_boot.erased = 1;
        if (!flash_write_page(addr, g_page) && !flash_write_page(addr, g_page)) {
            result(OTA_RESULT_FLASH);
            restart_transfer();
            return 0;
        }
        g_boot.pages++;
        g_fill = 0;
    }
    if (g_boot.expect < size) {
        return 0;
    }
    if (app_valid() && ((const app_header_t *)(APP_BASE + APP_HEADER_OFFSET))->crc == crc) {
        result(OTA_RESULT_OK);
        return 1;
    }
    result(OTA_RESULT_BAD_IMAGE);
    restart_transfer();
    return 0;
}

/* Returns only to start the (old, untouched) app. */
static void update(uint8_t have_app) {
    uint32_t t0 = g_ms, last = g_ms - REQUEST_EVERY_MS;

    restart_transfer();
    for (;;) {
        tick();
        IWDG->KR = 0xAAAA;          /* the app's watchdog runs on after its reset */
        if (((g_ms >> 7) & 1u)) {                             /* LED blinks fast: updating */
            GPIOA->BRR = 1u << 5;
        } else {
            GPIOA->BSRR = 1u << 5;
        }
        if (have_app && !g_boot.erased && g_ms - t0 > GIVE_UP_MS) {
            return;
        }
        if (g_ms - last >= REQUEST_EVERY_MS) {
            last = g_ms;
            request();
        }
        if (!rx_frame()) {
            continue;
        }
        uint8_t        cmd = g_rx[5];
        uint16_t       n   = (uint16_t)((g_rx[6] << 8) | g_rx[7]);
        const uint8_t *d   = g_rx + FRAME_HEADER;
        g_rx_len           = 0;

        if (cmd == OTA_CMD_NOTIFY) {
            uint8_t ok = 0;
            send(OTA_CMD_NOTIFY, &ok, 1);
            restart_transfer();
            last = g_ms - REQUEST_EVERY_MS;
        } else if (cmd == OTA_CMD_BLOCK) {
            if (on_block(d, n)) {
                for (uint32_t t = g_ms; g_ms - t < 20;) {     /* let the result frame leave */
                    tick();
                }
                NVIC_SystemReset();
            }
            last = g_ms - REQUEST_EVERY_MS;                   /* ask for the next block now */
        }
    }
}

int main(void);
int main(void) {
    clock_and_uart();
    g_boot.result = 0xFF;

    uint8_t asked = OTA_MAILBOX->magic == OTA_MAILBOX_MAGIC &&
                    OTA_MAILBOX->magic_inv == ~OTA_MAILBOX_MAGIC;
    OTA_MAILBOX->magic     = 0;
    OTA_MAILBOX->magic_inv = 0;

    g_boot.app_valid = app_valid();
    if (g_boot.app_valid && !asked) {
        jump_app();
    }
    update(g_boot.app_valid);
    jump_app();
    for (;;) {
    }
}

/* ---- startup -------------------------------------------------------------- */

/* Own minimal startup: no interrupts are used, so the vector table only needs
 * the stack, reset and the fault entries (a fault restarts and tries again). */
extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;

void Reset_Handler(void) {
    pins_safe();                  /* first: the gate pins float from reset until here */
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
