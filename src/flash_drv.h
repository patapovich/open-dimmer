/* PY32F002A flash page writes and CRC-32, shared by the app (settings) and the
 * bootloader (app image). Page = 128 bytes. An erase takes ~3.5 ms and a
 * program ~1 ms; the CPU (running from flash) and its interrupts stall
 * meanwhile. */
#ifndef FLASH_DRV_H
#define FLASH_DRV_H

#include <stdint.h>

#define FLASH_PAGE_BYTES   128u

/** Erase the page at addr (page aligned) and program 128 bytes from src
 * (word aligned). Returns 1 if it reads back equal. */
uint8_t  flash_write_page(uint32_t addr, const uint32_t *src);

/** CRC-32 (IEEE, as zlib), continued from crc (start with 0). */
uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);

#endif
