#include "host_boot.h"
#include "flash_drv.h"
#include <string.h>

uint32_t g_host_boot_sector[BOOT_SIZE / 4u];
uint32_t g_host_settings_start = SETTINGS_BASE_LEGACY;

uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    uint32_t c = ~crc;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

void host_boot_v1(void) {
    static const uint8_t code[4] = { 0x80, 0x22, 0x03, 0x4b };
    memset(g_host_boot_sector, 0xFF, sizeof(g_host_boot_sector));
    memcpy((uint8_t *)g_host_boot_sector + (BOOT_ID_ADDR - BOOT_BASE), code, sizeof(code));
}

void host_boot_v2(uint8_t features, uint32_t app_max) {
    boot_id_t id = { BOOT_ID_MAGIC, BOOT_VERSION, features, 0xFFFF, app_max, OTA_MAILBOX_RAM };
    memset(g_host_boot_sector, 0xFF, sizeof(g_host_boot_sector));
    memcpy((uint8_t *)g_host_boot_sector + (BOOT_ID_ADDR - BOOT_BASE), &id, sizeof(id));
}

void host_boot_word(uint32_t word) {
    g_host_mailbox.boot       = word;
    g_host_mailbox.boot_check = ~word;
}

uint32_t host_boot_word_now(void) {
    uint32_t w = g_host_mailbox.boot;
    return BOOT_WORD_VALID(w, g_host_mailbox.boot_check) ? w : 0xFFFFFFFFu;
}

uint32_t host_boot_sector_crc(void) {
    return crc32_update(0, (const uint8_t *)g_host_boot_sector, BOOT_SIZE);
}
