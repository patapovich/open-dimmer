#include "bootif.h"
#include "controls.h"
#include "dimmer.h"
#include "flash_drv.h"
#include "link_core.h"
#include "phase.h"
#include "py32f0xx.h"
#include "tuya.h"

#ifdef HOST_TEST
#define BOOT_SECTOR       ((const uint8_t *)g_host_boot_sector)
#define SETTINGS_START    g_host_settings_start
#else
extern const uint8_t _settings_start[];   /* py32f002a_app.ld: the settings ring */
#define BOOT_SECTOR       ((const uint8_t *)BOOT_BASE)
#define SETTINGS_START    ((uint32_t)(uintptr_t)_settings_start)
#endif

/* The link watchdog restarts normal builds only: the bootloader runs no probe for a bench build
 * (SIM, MEASURE, DIAG), so their restart would buy nothing. (LINK_WDG_TEST, bench test B11,
 * is a normal build's switch below the release: tuya.h refuses anything else.) */
#define LINK_WDG_BUILT    (!(SIM_ZC || MEASURE || DIAG_REPORT))

/* The app header's flags where the link watchdog runs (main.c g_app_header): no bench class. */
#define LINK_WDG_APP_FLAGS APP_FLAG_CONFIRMS

boot_info_t g_boot_info;

#if LINK_WDG_BUILT
static link_track_t g_link;
#endif

void bootif_init(void) {
    const boot_id_t *id    = (const boot_id_t *)(BOOT_SECTOR + (BOOT_ID_ADDR - BOOT_BASE));
    uint32_t         limit = APP_LIMIT_LEGACY;   /* v1: code at 0x10, no magic */

    g_boot_info.version  = OTA_BOOT_VERSION_V1;
    g_boot_info.features = 0;
    if (id->magic == BOOT_ID_MAGIC) {
        limit                = id->app_max;
        g_boot_info.version  = id->version;
        g_boot_info.features = id->features;
    }
    /* app_limit() = min(_settings_start - APP_BASE, the bootloader's): an image never reaches
     * the ring this app writes, nor past what the bootloader runs. A v3 bootloader puts an
     * image's bytes from BOOT_APP_MAX on past the ring at 0x08004800 (section 17): with that
     * ring its own ceiling holds. */
    if (limit > SETTINGS_START - APP_BASE &&
        !((g_boot_info.features & BOOT_FEAT_HIDDEN) && SETTINGS_START == SETTINGS_BASE_HIGH)) {
        limit = SETTINGS_START - APP_BASE;
    }
    g_boot_info.app_limit  = limit;
    g_boot_info.sector_crc = crc32_update(0, BOOT_SECTOR, BOOT_SIZE);

    volatile ota_mailbox_t *mb = OTA_MAILBOX;
    uint32_t                w  = mb->boot;
    g_boot_info.word = BOOT_WORD_VALID(w, mb->boot_check) ? w & 0xFFFFFFu : 0;
}

void bootif_poll(uint32_t now) {
    volatile ota_mailbox_t *mb = OTA_MAILBOX;
    uint32_t                w  = mb->boot;

    /* Both need a valid boot word: never under the v1 bootloader. */
    if (!BOOT_WORD_VALID(w, mb->boot_check)) {
        return;
    }
    /* Confirmation (section 7): tries 0, every other bit kept, once (then the tries are 0). A
     * run that cannot get here (a crash or hang in its first 30 s, a link that never answers)
     * stays unconfirmed; from the 3rd one in a row the bootloader opens rescue windows. */
    if (now >= BOOT_CONFIRM_MS && g_tuya_confirm >= BOOT_CONFIRM_QUERIES && (w & BOOT_TRIES_MASK)) {
        w             &= ~BOOT_TRIES_MASK;
        mb->boot       = w;
        mb->boot_check = ~w;
    }

#if LINK_WDG_BUILT
    uint8_t idle = dimmer_dark() && controls_idle();
    link_track(&g_link, now, LINK_WDG_TEST ? 0u : g_tuya_frames, idle);
    if (link_restart_due(&g_link, now, idle, LINK_WDG_APP_FLAGS,
                         g_boot_info.features, 1)) {
        /* The planned restart of an accepted offer (the gates go low first, although the light
         * is dark already; the state comes back after it), with the probe request and tries 0
         * in one word, so link restarts never move the counter. The update magic stays clear:
         * this is a probe, not an asked update. */
        bootif_restart(&mb->boot, (w & ~BOOT_TRIES_MASK) | BOOT_PROBE_REQ);
    }
#endif
}

void bootif_restart(volatile uint32_t *pair, uint32_t value) {
    phase_shutdown();
    dimmer_prepare_restart();
    pair[0] = value;
    pair[1] = ~value;
    NVIC_SystemReset();
}
