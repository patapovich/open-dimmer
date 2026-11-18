/* The bootloader's side for host tests that link src/bootif.c (built with -DHOST_TEST): the
 * 4 KB boot sector (a v1 bootloader, or a v2 one with its boot_id at 0x10), the settings start
 * of the app linker script, and the mailbox's boot word pair. Also the CRC-32 of the flash
 * driver (a reference implementation: the C tests do not link src/flash_drv.c). */
#ifndef HOST_BOOT_H
#define HOST_BOOT_H

#include <stdint.h>
#include "bootif.h"
#include "ota_proto.h"

/** Boot sector of the released v1 bootloader: code at 0x10 (80 22 03 4b), no boot_id. */
void host_boot_v1(void);
/** Boot sector of a v2 bootloader with this boot_id (version 2, features, app_max). */
void host_boot_v2(uint8_t features, uint32_t app_max);
/** The boot word pair as a v2 bootloader leaves it: word, ~word. */
void host_boot_word(uint32_t word);
/** The boot word if the pair is valid, else 0xFFFFFFFF (never a valid word: tag 0xFF). */
uint32_t host_boot_word_now(void);
/** CRC-32 (zlib) of the boot sector as it is now, computed here. */
uint32_t host_boot_sector_crc(void);

#endif
