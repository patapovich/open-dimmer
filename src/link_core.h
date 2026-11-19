/* Link watchdog of the app (docs/BOOTLOADER.md section 9, item 11; user decision U4), the
 * hardware-free part, unit tested on the PC (tests/test_link_wdg.c). Inline: bootif.c calls
 * it once per main-loop pass.
 *
 * An app whose UART link to the module is dead (its own bug, or the module's) hears no frame
 * any more. After LINK_WDG_SILENCE_MS (30 min) without one, in a run at least
 * LINK_WDG_UPTIME_MS (4 h) old, once every channel has been off and no input pressed for
 * LINK_WDG_QUIET_MS (1 min), it restarts
 * with the boot word's BOOT_PROBE_REQ set: the v2 bootloader then runs the power-on probe on
 * that warm boot (newer build only, 3 s window) and the light state comes back by the restore
 * rule (off). Every restart begins a new run, so link restarts are at least 4 h apart.
 *
 * Armed only under a bootloader that honours the request (boot_id features BOOT_FEAT_LINK), with
 * a valid boot word, in a normal build: the v1 bootloader and the bench builds (SIM, MEASURE,
 * DIAG: the bootloader skips their probe) never get the restart. */
#ifndef LINK_CORE_H
#define LINK_CORE_H

#include <stdint.h>
#include "ota_proto.h"

/* Bench test B11 (docs/BOOTLOADER.md section 14; make LINK_WDG_TEST=1 FW_PREV=1 FW_TEST=n, a
 * build below the release: src/tuya.h refuses it otherwise): 2 min of silence in a run of
 * 5 min, and the watchdog sees no frame at all (they are still handled). */
#ifndef LINK_WDG_TEST
#define LINK_WDG_TEST        0
#endif
#if LINK_WDG_TEST
#define LINK_SILENCE_MS      (2u * 60u * 1000u)
#define LINK_UPTIME_MS       (5u * 60u * 1000u)
#else
#define LINK_SILENCE_MS      LINK_WDG_SILENCE_MS
#define LINK_UPTIME_MS       LINK_WDG_UPTIME_MS
#endif
#define LINK_QUIET_MS        LINK_WDG_QUIET_MS

typedef struct {
    uint32_t frames;     /* the module's frame count (g_tuya_frames) when it last changed */
    uint32_t heard_ms;   /* when that was, or the start of the run (main.c g_ms, 0 at the start) */
    uint8_t  aged;       /* this run is LINK_UPTIME_MS old: stays set, so the ms counter's wrap
                            after 49.7 days does not take it back */
    uint32_t busy_ms;    /* the last pass with a light or an input (not idle), or the start */
} link_track_t;          /* zeroed: the start of a run */

/** Every main-loop pass: the time, the count of well-formed frames received so far and whether
 * the pass is idle (every channel dark, no input pressed). A new frame since the last pass starts
 * the silence again; noise bytes form no frame. A pass that is not idle starts the quiet time
 * again. */
static inline void link_track(link_track_t *t, uint32_t now, uint32_t frames, uint8_t idle) {
    if (!idle) {
        t->busy_ms = now;
    }
    if (frames != t->frames) {
        t->frames   = frames;
        t->heard_ms = now;
    }
    if (now >= LINK_UPTIME_MS) {
        t->aged = 1;
    }
}

/** The decision, a pure function: 1 when the link watchdog restarts now. now - t->heard_ms is
 * the silence (wrap-safe up to 49.7 days of it), now - t->busy_ms the quiet time; idle: every
 * channel is off and dark (no fade, no hold) and no input is pressed (dimmer_dark,
 * controls_idle), the same value link_track got in this pass; app_flags: the app header's
 * (never a bench build); boot_features: the bootloader's (boot_id, 0 without one); word_valid:
 * the mailbox's boot word pair is valid. */
static inline uint8_t link_restart_due(const link_track_t *t, uint32_t now, uint8_t idle,
                                       uint8_t app_flags, uint8_t boot_features,
                                       uint8_t word_valid) {
    return now - t->heard_ms >= LINK_SILENCE_MS && t->aged && idle &&
           now - t->busy_ms >= LINK_QUIET_MS &&
           !(app_flags & APP_FLAGS_BENCH) && (boot_features & BOOT_FEAT_LINK) && word_valid;
}

#endif
