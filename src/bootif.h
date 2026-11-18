/* The app's side of the bootloader (docs/BOOTLOADER.md sections 1, 3, 7 and 9):
 *  - what the bootloader says about itself (boot_id at 0x08000010, v2 only): the largest image
 *    an offer may bring, the bootloader version for the identity answer, its features;
 *  - DP 136 (CRC-32 of the 4 KB boot sector) and DP 137 (the boot word at start), sent with the
 *    health report;
 *  - the confirmation of a run (section 7): tries 0 in the boot word, every other bit kept;
 *  - the link watchdog (section 9, item 11; link_core.h): its restart with BOOT_PROBE_REQ.
 * Under the v1 bootloader (no boot_id, the boot word never valid) it reports version 1, keeps
 * the 12 KB limit, confirms nothing and never arms the link watchdog. */
#ifndef BOOTIF_H
#define BOOTIF_H

#include <stdint.h>
#include "ota_proto.h"

typedef struct {
    uint32_t sector_crc;   /* DP 136: CRC-32 of 0x08000000..0x08000FFF (released v1: 0xEAC45031) */
    uint32_t word;         /* DP 137: the boot word at start, bits 23..0 (probe time, info, LINK,
                              tries); 0 when the word was not valid (v1 bootloader) */
    uint32_t app_limit;    /* offers: the largest image that fits the bootloader and the ring */
    uint8_t  version;      /* identity byte 9: boot_id.version, OTA_BOOT_VERSION_V1 without one */
    uint8_t  features;     /* boot_id.features (BOOT_FEAT_*), 0 without a boot_id */
} boot_info_t;

extern boot_info_t g_boot_info;

/** At start, after the watchdog: reads the boot_id and the boot word, CRCs the boot sector
 * (~15 ms at 24 MHz). */
void bootif_init(void);
/** Main loop, every pass (now: main.c g_ms): confirms the run once it may (BOOT_CONFIRM_MS of
 * uptime and g_tuya_confirm), and in normal builds runs the link watchdog, whose restart does
 * not return. */
void bootif_poll(uint32_t now);
/** The planned restart (an accepted offer, the link watchdog): gates low, a due save and the
 * light state into the mailbox (dimmer_prepare_restart: it comes back after the restart), then
 * pair[0] = value and pair[1] = ~value (the update magic, or the boot word), and the reset.
 * Returns only on the host. */
void bootif_restart(volatile uint32_t *pair, uint32_t value);

#ifdef HOST_TEST
/* Host tests: the boot sector and the app linker script's _settings_start are the test's. */
extern uint32_t g_host_boot_sector[BOOT_SIZE / 4u];
extern uint32_t g_host_settings_start;
#endif

#endif
