/* The real src/flash_drv.c for the bootloader harness (tests/boot/harness.c, built with
 * REAL_FLASH_DRV): the driver is compiled as C++ against the register model of
 * tests/host/flash_model.hpp, and this file gives the C harness its knobs. The model runs with
 * the chip's timing (an erase ~3.5 ms, a program ~1 ms, ~8 cycles per busy-wait pass at 24 MHz),
 * every SR read reported to the harness's clock. */
#include "py32f0xx.h"   /* tests/host: the flash model */

host_flash_model host_flash;
host_flash_regs  host_flash_regs_inst;
host_rcc_regs    host_rcc_regs_inst;

static void (*g_advance)(uint32_t ns);
static uint8_t g_wrperr_always;

static void on_sr_read(uint32_t ns) {
    if (g_wrperr_always) {
        host_flash.erase_raises = FLASH_SR_WRPERR;   /* every erase fails */
    }
    g_advance(ns);
}

extern "C" {

/* sr: SR flags at reset (stale flags must not fail a write). wrperr: 1 every erase raises
 * WRPERR, 2 only the first erase (the model clears erase_raises once it is used). */
void drv_model_init(void (*advance_ns)(uint32_t), uint32_t sr, int bsy_stuck, int wrperr) {
    host_flash               = host_flash_model();
    host_flash.cr            = FLASH_CR_LOCK;
    host_flash.sr            = sr;
    host_flash.erase_ns      = 3500000u;
    host_flash.program_ns    = 1000000u;
    host_flash.poll_ns       = 333u;
    host_flash.bsy_stuck     = (uint8_t)bsy_stuck;
    host_flash.advance_ns    = on_sr_read;
    g_advance                = advance_ns;
    g_wrperr_always          = wrperr == 1;
    host_flash.erase_raises  = wrperr ? FLASH_SR_WRPERR : 0;
    host_rcc_regs_inst.ICSCR = 4u << RCC_ICSCR_HSI_FS_Pos;   /* 24 MHz, as the bootloader sets */
}

void drv_model_stats(uint32_t *erases, uint32_t *programs, uint32_t *unlocks, uint32_t *locked_idle) {
    *erases      = host_flash.erases;
    *programs    = host_flash.programs;
    *unlocks     = host_flash.unlocks;
    *locked_idle = (host_flash.cr & FLASH_CR_LOCK) && !(host_flash.cr & (FLASH_CR_PER | FLASH_CR_PG));
}

}
