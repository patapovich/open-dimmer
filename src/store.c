#include "store.h"
#include "py32f0xx.h"
#include <stddef.h>
#include <string.h>

#define PAGE_SIZE       128u
#define STORE_MAGIC     0x5A445331u   /* "ZDS1" */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  data[STORE_DATA_SIZE];
    uint32_t crc;
} record_t;

_Static_assert(sizeof(record_t) <= PAGE_SIZE, "record must fit a page");

extern const uint8_t _settings_start[];
extern const uint8_t _settings_end[];

#define STORE_PAGES     ((uint32_t)(_settings_end - _settings_start) / PAGE_SIZE)

/* Factory flash timing for each HSI setting (RCC_ICSCR.HSI_FS), as in the
 * vendor driver: 4, 8, 16, 22.12, 24 MHz. */
static const uint32_t k_timing_addr[8] = { 0x1FFF0F1C, 0x1FFF0F30, 0x1FFF0F44, 0x1FFF0F58,
                                           0x1FFF0F6C, 0x1FFF0F1C, 0x1FFF0F1C, 0x1FFF0F1C };

static int16_t  g_newest = -1;   /* page index of the newest valid record */
static uint32_t g_seq;
static uint16_t g_saves;

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (uint8_t k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static const record_t *page_record(uint32_t page) {
    return (const record_t *)(_settings_start + page * PAGE_SIZE);
}

static uint8_t record_valid(const record_t *r) {
    return r->magic == STORE_MAGIC &&
           r->crc == crc32((const uint8_t *)r, offsetof(record_t, crc));
}

uint8_t store_load(uint8_t *data) {
    g_newest = -1;
    for (uint32_t i = 0; i < STORE_PAGES; i++) {
        const record_t *r = page_record(i);
        if (!record_valid(r)) {
            continue;
        }
        if (g_newest < 0 || (int32_t)(r->seq - g_seq) > 0) {
            g_newest = (int16_t)i;
            g_seq    = r->seq;
        }
    }
    if (g_newest < 0) {
        return 0;
    }
    memcpy(data, page_record((uint32_t)g_newest)->data, STORE_DATA_SIZE);
    return 1;
}

/* ---- flash controller ---------------------------------------------------- */

static void flash_set_timing(void) {
    uint32_t        fs = (RCC->ICSCR & RCC_ICSCR_HSI_FS) >> RCC_ICSCR_HSI_FS_Pos;
    const uint32_t *t  = (const uint32_t *)k_timing_addr[fs & 7u];

    FLASH->TS0     = t[0] & 0xFFu;
    FLASH->TS1     = (t[0] >> 16) & 0x1FFu;
    FLASH->TS3     = (t[0] >> 8) & 0xFFu;
    FLASH->TS2P    = t[1] & 0xFFu;
    FLASH->TPS3    = (t[1] >> 16) & 0x7FFu;
    FLASH->PERTPE  = t[2] & 0x1FFFFu;
    FLASH->SMERTPE = t[3] & 0x1FFFFu;
    FLASH->PRGTPE  = t[4] & 0xFFFFu;
    FLASH->PRETPE  = (t[4] >> 16) & 0x3FFFu;
}

static void flash_unlock(void) {
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

static void flash_lock(void) {
    FLASH->CR |= FLASH_CR_LOCK;
}

/* Returns 1 when the controller finished without an error. */
static uint8_t flash_wait(void) {
    uint32_t n = 2000000u;
    while (FLASH->SR & FLASH_SR_BSY) {
        if (--n == 0) {
            return 0;
        }
    }
    uint32_t sr = FLASH->SR;
    FLASH->SR   = FLASH_SR_EOP | FLASH_SR_WRPERR | FLASH_SR_OPTVERR;   /* write 1 to clear */
    return (sr & (FLASH_SR_WRPERR | FLASH_SR_OPTVERR)) == 0;
}

/* Page program: 32 word writes into the page buffer with no other flash
 * access in between; PGSTRT goes before the last word. Runs from RAM with
 * interrupts off, like the vendor driver. */
__attribute__((section(".RamFunc"), noinline, long_call))
static void program_page_ram(uint32_t addr, const uint32_t *src) {
    volatile uint32_t *dst = (volatile uint32_t *)addr;

    __disable_irq();
    for (uint32_t i = 0; i < PAGE_SIZE / 4u; i++) {
        if (i == PAGE_SIZE / 4u - 1u) {
            FLASH->CR |= FLASH_CR_PGSTRT;
        }
        dst[i] = src[i];
    }
    __enable_irq();
}

static uint8_t write_page(uint32_t addr, const uint32_t *src) {
    uint8_t ok;

    flash_set_timing();
    flash_unlock();
    ok = flash_wait();

    if (ok) {
        FLASH->CR |= FLASH_CR_PER;
        *(volatile uint32_t *)addr = 0xFFu;      /* starts the page erase */
        ok = flash_wait();
        FLASH->CR &= ~FLASH_CR_PER;
    }
    if (ok) {
        FLASH->CR |= FLASH_CR_PG;
        program_page_ram(addr, src);
        ok = flash_wait();
        FLASH->CR &= ~FLASH_CR_PG;
    }
    flash_lock();
    return ok && memcmp((const void *)addr, src, PAGE_SIZE) == 0;
}

uint8_t store_save(const uint8_t *data) {
    static uint32_t page_buf[PAGE_SIZE / 4u];
    record_t       *r = (record_t *)page_buf;

    memset(page_buf, 0xFF, sizeof(page_buf));
    r->magic = STORE_MAGIC;
    r->seq   = g_seq + 1u;
    memcpy(r->data, data, STORE_DATA_SIZE);
    r->crc = crc32((const uint8_t *)r, offsetof(record_t, crc));

    /* Try the next page; if it does not verify (worn out), the one after. */
    uint32_t page = g_newest < 0 ? 0u : ((uint32_t)g_newest + 1u) % STORE_PAGES;
    for (uint8_t attempt = 0; attempt < 2; attempt++) {
        if (write_page((uint32_t)(_settings_start + page * PAGE_SIZE), page_buf)) {
            g_newest = (int16_t)page;
            g_seq    = r->seq;
            g_saves++;
            return 1;
        }
        page = (page + 1u) % STORE_PAGES;
    }
    return 0;
}

uint16_t store_save_count(void) {
    return g_saves;
}
