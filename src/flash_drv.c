#include "flash_drv.h"
#include "py32f0xx.h"

/* Factory flash timing for each HSI setting (RCC_ICSCR.HSI_FS), as in the
 * vendor driver: 4, 8, 16, 22.12, 24 MHz. */
static const uint32_t k_timing_addr[8] = { 0x1FFF0F1C, 0x1FFF0F30, 0x1FFF0F44, 0x1FFF0F58,
                                           0x1FFF0F6C, 0x1FFF0F1C, 0x1FFF0F1C, 0x1FFF0F1C };

uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    uint32_t c = ~crc;
    while (n--) {
        c ^= *p++;
        for (uint8_t k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

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

/* Returns 1 when the controller finished without a write error since the
 * flags were last cleared, and clears them. WRPERR is the only error a page
 * erase or program raises; OPTVERR tells of the option byte load at reset,
 * and the read-back after programming checks the page anyway. */
static uint8_t flash_wait(void) {
    uint32_t n = 2000000u;
    while (FLASH->SR & FLASH_SR_BSY) {
        if (--n == 0) {
            return 0;
        }
    }
    uint32_t sr = FLASH->SR;
    FLASH->SR   = FLASH_SR_EOP | FLASH_SR_WRPERR | FLASH_SR_OPTVERR;   /* write 1 to clear */
    return (sr & FLASH_SR_WRPERR) == 0;
}

/* Page program: 32 word writes into the page buffer with no other flash
 * access in between; PGSTRT goes before the last word. Runs from RAM with
 * interrupts off, like the vendor driver. */
__attribute__((section(".RamFunc"), noinline, long_call))
static void program_page_ram(uint32_t addr, const uint32_t *src) {
    volatile uint32_t *dst = (volatile uint32_t *)addr;

    __disable_irq();
    for (uint32_t i = 0; i < FLASH_PAGE_BYTES / 4u; i++) {
        if (i == FLASH_PAGE_BYTES / 4u - 1u) {
            FLASH->CR |= FLASH_CR_PGSTRT;
        }
        dst[i] = src[i];
    }
    __enable_irq();
}

uint8_t flash_write_page(uint32_t addr, const uint32_t *src) {
    uint8_t         ok;
    const uint32_t *now = (const uint32_t *)addr;
    uint32_t        i;

    for (i = 0; i < FLASH_PAGE_BYTES / 4u && now[i] == src[i]; i++) {
    }
    if (i == FLASH_PAGE_BYTES / 4u) {
        return 1;                   /* already there: no erase (wear) */
    }

    flash_set_timing();
    /* Flags left by an earlier operation or by the reset are not this
     * write's: cleared first, so each wait sees only its own operation's. */
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_WRPERR | FLASH_SR_OPTVERR;
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
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
    FLASH->CR |= FLASH_CR_LOCK;
    const uint32_t *got = (const uint32_t *)addr;
    for (uint32_t i = 0; ok && i < FLASH_PAGE_BYTES / 4u; i++) {
        ok = got[i] == src[i];
    }
    return ok;
}
