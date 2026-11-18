/* Register model of the PY32F002A flash controller for the host tests of
 * src/flash_drv.c (tests/test_flash_drv.cpp, and the real-driver variant of the
 * bootloader harness tests/boot), built as C++ so that register reads and
 * writes can be seen. SR flags are write-1-to-clear, as on the chip. A test can
 * make the erase or the program raise an SR flag, or leave the page wrong. By
 * default an operation finishes at once (BSY never seen); with erase_ns /
 * program_ns BSY stays set for that long after the operation starts, every SR
 * read costs poll_ns (one pass of the driver's busy wait) and is reported to
 * advance_ns, and bsy_stuck makes BSY never clear. The page itself and the
 * factory timing words are plain memory mapped at their chip addresses. */
#ifndef HOST_FLASH_MODEL_HPP
#define HOST_FLASH_MODEL_HPP

#include <stdint.h>

#define FLASH_SR_EOP       (1u << 0)
#define FLASH_SR_WRPERR    (1u << 4)
#define FLASH_SR_OPTVERR   (1u << 15)
#define FLASH_SR_BSY       (1u << 16)
#define FLASH_CR_PG        (1u << 0)
#define FLASH_CR_PER       (1u << 1)
#define FLASH_CR_PGSTRT    (1u << 19)
#define FLASH_CR_LOCK      (1u << 31)
#define FLASH_KEY1         0x45670123u
#define FLASH_KEY2         0xCDEF89ABu
#define RCC_ICSCR_HSI_FS_Pos 13u
#define RCC_ICSCR_HSI_FS   (7u << RCC_ICSCR_HSI_FS_Pos)

struct host_flash_model {
    uint32_t sr;
    uint32_t cr;
    uint8_t  key_step;       /* KEY1 seen */
    uint32_t unlocks;        /* completed key sequences */
    uint32_t sr_clears;      /* writes to SR */
    uint32_t erases;         /* PER set (an erase starts) */
    uint32_t programs;       /* PGSTRT set (a program starts) */
    uint32_t erase_raises;   /* SR flags the next erase raises */
    uint32_t program_raises; /* SR flags the next program raises */
    uint32_t pending;        /* raised when the running operation ends */
    uint32_t erase_ns;       /* BSY time of an erase, of a program (0: at once) */
    uint32_t program_ns;
    uint32_t poll_ns;        /* model time one SR read costs */
    uint8_t  bsy_stuck;      /* BSY never clears */
    uint64_t now_ns;         /* model time, advanced by SR reads */
    uint64_t busy_until_ns;
    uint64_t sr_reads;
    void   (*advance_ns)(uint32_t ns);   /* told of every SR read's cost (the owner's clock) */
};
extern host_flash_model host_flash;

struct host_sr_reg {
    operator uint32_t() const {
        host_flash.sr_reads++;
        host_flash.now_ns += host_flash.poll_ns;
        if (host_flash.advance_ns) {
            host_flash.advance_ns(host_flash.poll_ns);
        }
        if (host_flash.bsy_stuck || host_flash.now_ns < host_flash.busy_until_ns) {
            return host_flash.sr | FLASH_SR_BSY;
        }
        host_flash.sr |= host_flash.pending;   /* the operation has ended */
        host_flash.pending = 0;
        return host_flash.sr;
    }
    host_sr_reg &operator=(uint32_t w) {
        host_flash.sr_clears++;
        host_flash.sr &= ~(w & (FLASH_SR_EOP | FLASH_SR_WRPERR | FLASH_SR_OPTVERR));
        return *this;
    }
};

struct host_cr_reg {
    operator uint32_t() const { return host_flash.cr; }
    host_cr_reg &operator|=(uint32_t b) {
        if ((b & FLASH_CR_PER) && !(host_flash.cr & FLASH_CR_LOCK)) {
            host_flash.erases++;
            host_flash.pending |= host_flash.erase_raises | FLASH_SR_EOP;
            host_flash.erase_raises = 0;
            host_flash.busy_until_ns = host_flash.now_ns + host_flash.erase_ns;
        }
        if ((b & FLASH_CR_PGSTRT) && (host_flash.cr & FLASH_CR_PG) && !(host_flash.cr & FLASH_CR_LOCK)) {
            host_flash.programs++;
            host_flash.pending |= host_flash.program_raises | FLASH_SR_EOP;
            host_flash.program_raises = 0;
            host_flash.busy_until_ns = host_flash.now_ns + host_flash.program_ns;
        }
        host_flash.cr |= b & ~FLASH_CR_PGSTRT;
        return *this;
    }
    host_cr_reg &operator&=(uint32_t b) {
        host_flash.cr &= b | FLASH_CR_LOCK;     /* only the key sequence clears LOCK */
        return *this;
    }
};

struct host_key_reg {
    host_key_reg &operator=(uint32_t k) {
        if (k == FLASH_KEY1) {
            host_flash.key_step = 1;
        } else if (k == FLASH_KEY2 && host_flash.key_step == 1) {
            host_flash.key_step = 0;
            host_flash.cr &= ~FLASH_CR_LOCK;
            host_flash.unlocks++;
        } else {
            host_flash.key_step = 0;
        }
        return *this;
    }
};

struct host_flash_regs {
    host_key_reg KEYR;
    host_sr_reg  SR;
    host_cr_reg  CR;
    uint32_t     TS0, TS1, TS2P, TPS3, TS3, PERTPE, SMERTPE, PRGTPE, PRETPE;
};
struct host_rcc_regs {
    uint32_t ICSCR;
};
extern host_flash_regs host_flash_regs_inst;
extern host_rcc_regs   host_rcc_regs_inst;

#define FLASH (&host_flash_regs_inst)
#define RCC   (&host_rcc_regs_inst)

#endif
