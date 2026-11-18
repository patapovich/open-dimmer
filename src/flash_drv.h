/* PY32F002A flash page writes and CRC-32, shared by the app (settings) and the
 * bootloader (app image). Page = 128 bytes. An erase takes ~3.5 ms and a
 * program ~1 ms; the CPU (running from flash) and its interrupts stall
 * meanwhile. */
#ifndef FLASH_DRV_H
#define FLASH_DRV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {   /* host tests build the driver as C++ (tests/test_flash_drv.cpp, tests/boot) */
#endif

#define FLASH_PAGE_BYTES   128u

/** Erase the page at addr and program 128 bytes from src (word aligned).
 * Returns 1 if it reads back equal; a page that already holds src is not
 * erased. Refused (0, the controller untouched) unless addr is page aligned
 * and the page lies inside [_flash_write_start, _flash_write_end): linker
 * symbols, the settings region in the app and the app area in the
 * bootloader, so neither can write the other's flash by a bug. */
uint8_t  flash_write_page(uint32_t addr, const uint32_t *src);

/** CRC-32 (IEEE, as zlib), continued from crc (start with 0). */
uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif
