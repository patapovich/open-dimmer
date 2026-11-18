/* Host test of src/flash_drv.c (the settings writes of the app, and the
 * bootloader's page writes) against the register model in
 * tests/host/flash_model.hpp. Built as C++ so that every SR / CR / KEYR access
 * goes through the model; the page and the factory timing words are memory
 * mapped at their chip addresses. Run: make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "flash_drv.h"
#include "py32f0xx.h"

host_flash_model host_flash;
host_flash_regs  host_flash_regs_inst;
host_rcc_regs    host_rcc_regs_inst;

static int g_failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            g_failures++;                                                \
        }                                                                \
    } while (0)

#define TIMING_BASE  0x1FFF0000u
#define SETTINGS     0x08004000u
#define PAGE_ADDR    (SETTINGS + 0x80u)

static uint32_t g_src[FLASH_PAGE_BYTES / 4u];

static void map_at(uintptr_t addr, size_t n) {
    void *p = mmap((void *)addr, n, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)addr) {
        printf("cannot map 0x%08lx for the flash model\n", (unsigned long)addr);
        exit(1);
    }
}

static uint32_t *page(void) {
    return (uint32_t *)(uintptr_t)PAGE_ADDR;
}

static void model_reset(void) {
    memset(&host_flash, 0, sizeof(host_flash));
    host_flash.cr = FLASH_CR_LOCK;
    memset(page(), 0xFF, FLASH_PAGE_BYTES);
}

static void fill(uint32_t seed) {
    for (uint32_t i = 0; i < FLASH_PAGE_BYTES / 4u; i++) {
        g_src[i] = seed * 0x01000193u + i;
    }
}

static void check_idle_and_locked(void) {
    CHECK(host_flash.cr & FLASH_CR_LOCK);
    CHECK((host_flash.cr & (FLASH_CR_PER | FLASH_CR_PG)) == 0);
}

/* Flags an earlier operation (or the reset) left are not this write's. */
static void test_stale_flags_do_not_fail_a_write(void) {
    model_reset();
    host_flash.sr = FLASH_SR_WRPERR | FLASH_SR_OPTVERR | FLASH_SR_EOP;
    fill(1);
    CHECK(flash_write_page(PAGE_ADDR, g_src) == 1);
    CHECK(memcmp(page(), g_src, FLASH_PAGE_BYTES) == 0);
    CHECK(host_flash.erases == 1 && host_flash.programs == 1);
    CHECK(host_flash.sr == 0);
    check_idle_and_locked();
}

static void test_erase_error_fails_and_programs_nothing(void) {
    model_reset();
    host_flash.erase_raises = FLASH_SR_WRPERR;
    fill(2);
    CHECK(flash_write_page(PAGE_ADDR, g_src) == 0);
    CHECK(host_flash.erases == 1 && host_flash.programs == 0);
    CHECK(host_flash.sr == 0);             /* cleared: the next write starts clean */
    check_idle_and_locked();
}

static void test_program_error_fails(void) {
    model_reset();
    host_flash.program_raises = FLASH_SR_WRPERR;
    fill(3);
    CHECK(flash_write_page(PAGE_ADDR, g_src) == 0);
    CHECK(host_flash.erases == 1 && host_flash.programs == 1);
    check_idle_and_locked();
    /* The error was that write's only: the next one succeeds. */
    CHECK(flash_write_page(PAGE_ADDR, g_src) == 1);
    CHECK(memcmp(page(), g_src, FLASH_PAGE_BYTES) == 0);
    check_idle_and_locked();
}

/* OPTVERR tells of the option byte load, not of a page operation. */
static void test_optverr_is_not_a_write_error(void) {
    model_reset();
    host_flash.program_raises = FLASH_SR_OPTVERR;
    fill(4);
    CHECK(flash_write_page(PAGE_ADDR, g_src) == 1);
    CHECK(memcmp(page(), g_src, FLASH_PAGE_BYTES) == 0);
}

/* A page that is already right is not erased (wear), and the controller is
 * not touched at all. */
static void test_identical_page_is_left_alone(void) {
    model_reset();
    fill(5);
    memcpy(page(), g_src, FLASH_PAGE_BYTES);
    host_flash.sr = FLASH_SR_WRPERR;
    CHECK(flash_write_page(PAGE_ADDR, g_src) == 1);
    CHECK(host_flash.erases == 0 && host_flash.unlocks == 0 && host_flash.sr_clears == 0);
}

/* Factory timing for the HSI setting in use (24 MHz: the fifth entry). */
static void test_timing_from_the_factory_words(void) {
    uint32_t *t = (uint32_t *)(uintptr_t)0x1FFF0F6Cu;
    t[0] = 0x00AB12CDu;
    t[1] = 0x07FF00EEu;
    t[2] = 0x0001FFFFu;
    t[3] = 0x00012345u;
    t[4] = 0x3FFFABCDu;
    host_rcc_regs_inst.ICSCR = 4u << RCC_ICSCR_HSI_FS_Pos;
    model_reset();
    fill(6);
    CHECK(flash_write_page(PAGE_ADDR, g_src) == 1);
    CHECK(host_flash_regs_inst.TS0 == 0xCDu);
    CHECK(host_flash_regs_inst.TS1 == 0xABu);
    CHECK(host_flash_regs_inst.TS3 == 0x12u);
    CHECK(host_flash_regs_inst.TS2P == 0xEEu);
    CHECK(host_flash_regs_inst.TPS3 == 0x7FFu);
    CHECK(host_flash_regs_inst.PERTPE == 0x1FFFFu);
    CHECK(host_flash_regs_inst.SMERTPE == 0x12345u);
    CHECK(host_flash_regs_inst.PRGTPE == 0xABCDu);
    CHECK(host_flash_regs_inst.PRETPE == 0x3FFFu);
}

static void test_crc32_is_zlibs(void) {
    const uint8_t *s = (const uint8_t *)"123456789";
    CHECK(crc32_update(0, s, 9) == 0xCBF43926u);
    CHECK(crc32_update(crc32_update(0, s, 4), s + 4, 5) == 0xCBF43926u);
    CHECK(crc32_update(0, s, 0) == 0);
}

int main(void) {
    map_at(TIMING_BASE, 4096);
    map_at(SETTINGS, 4096);
    test_stale_flags_do_not_fail_a_write();
    test_erase_error_fails_and_programs_nothing();
    test_program_error_fails();
    test_optverr_is_not_a_write_error();
    test_identical_page_is_left_alone();
    test_timing_from_the_factory_words();
    test_crc32_is_zlibs();
    if (g_failures) {
        printf("test_flash_drv: %d FAILED\n", g_failures);
        return 1;
    }
    printf("test_flash_drv: all passed\n");
    return 0;
}
