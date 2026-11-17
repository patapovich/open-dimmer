# ZDMS16-2 own PY32 firmware: status (2026-11-17, end of day)

Own firmware for the PY32F002A dimming MCU of the AVATTO ZDMS16-2, updated over Zigbee by
the own ZTU firmware (fork `patapovich/tuya-zigbee-switch`, branch `home-dimmer`, local
checkout `~/projects/tzs-diag`).

## Where things are

| What | Version / place |
|---|---|
| Spare module (<spare IEEE>, test lamp on L1, push button L–S1) | ZTU 1.1.3-74eb831f (0x11033018), MCU **2.1.9 own** |
| House modules (4) | stock PY32 MCU, own ZTU; **not migrated** |
| PY32 source | this repo (github.com/patapovich/zdms16-py32, local checkout `~/projects/zdms16-py32/fw`) |
| ZTU + converter | `~/projects/tzs-diag`, branch `home-dimmer` (HEAD b123d6fb, pushed) |
| Z2M converter deployed | `/config/zigbee2mqtt/external_converters/switch_custom.js` (generated from home-dimmer) |
| SWD tools | `tools/swd/install.sh` (one-contact install, docs/INSTALL.md; untested on a stock module yet), the single steps beside it (`.venv` and `pack/` live in `~/projects/zdms16-py32`) |
| HA websocket / expose check | `tools/ha/ha_ws.py`, `tools/ha/expose_dump.js` |
| HA-side tools | `~/ha-claude/zdms16_mcu_log.py` (raw MCU link log), `zdms16_health.py` (health report), `zdms16_measure_read.py` (diag build stats), `zdms16_ota_series.sh` + `zdms16_ota_guard.sh` (fast OTA) |
| Docs | `docs/REVIEW-2026-11-17.md`, `docs/ZC-DELAY-MEASUREMENT.md`, `docs/stock-firmware-analysis.md` |

## Flash layout and budget

Bootloader 4 KB @0x08000000, app 12 KB @0x08001000 (header at +0xC0), settings 4 KB
@0x08004000 (32-page ring, record v2 "ZDS2" 40 bytes; v1 "ZDS1" 20 bytes still read).
App 2.1.9: 11 476 bytes (~0.8 KB free). DIAG / MEASURE builds ~12.1 KB (health report and
raw timing compiled out there).

## DPs (own firmware; stock ones 1..14 unchanged)

| DP | Meaning |
|---|---|
| 1..6 / 7..12 | channel 1 / 2: on, level, min, switch type, max, countdown (stock layout) |
| 14 | power-on, both channels (written: both; reported: channel 1's) |
| 101 / 102 | dimming curve: linear, led (default for new channels), led_soft, incandescent, led_deep, led_deeper |
| 103 / 104 | power-on per channel |
| 105 / 106 | wall switch gesture (momentary type), event: 1 single, 2 double, 3 triple, 4 hold, 5 release |
| 130..134 | health every 2 min: uptime min, reset cause (RCC_CSR >> 24), lock losses, ignored edges, saves |
| 150..154 / 155..159 | per channel: fade on ms, fade off ms, soft dim end %, hold ms, hold dim end % (saved) |
| 110..126 | DIAG / MEASURE builds only (statistics every 10 s) |
| 0xA0..0xA3 | own commands: update notify / block / result, identity |

Min and max are physical conduction limits for every curve (same meaning as the stock MCU).

## Behaviour (as tuned with the lamp)

- Switch on from dark: starts at min, 950 ms to full. Off: 950 ms, the lowest 12 % above min
  slow to 1 per half cycle, 80 ms at min, then dark.
- Momentary switch: tap from dark → quickly to the stored level; hold from dark → rises
  slowly from min (pick a very dim level); hold while lit → full rate up (no slow start),
  slow near min going down; release stops at once.
- Module button: both channels (any on → all off). ≥ 3 s: rejoin.
- 50 and 60 Hz: the zero-cross tracker takes 40..70 Hz; fade and hold rates scale with the
  measured half period (1/64 steps per 10 ms), so the times in ms hold on both (tested).
- The board's S1 input stays low ~0.1 s after release: a fast double tap merges in
  hardware; doubles need ~0.2 s gaps.

## Build and deploy (CI / CD)

- **CI** (`.github/workflows/ci.yml`, every push / PR): host tests; normal, DIAG, MEASURE and
  SIM builds with the pinned SDK; curve tables vs generator; ZTU embedding refuses bench and
  measurement builds; size report; artifacts.
- **Release:** bump `FW_VERSION` in `src/tuya.h`, commit, `tools/release/release.sh`: checks,
  tags `vX.Y.Z` (must match the image), CI publishes the release (image, full image,
  bootloader, `secondary_mcu_image.c`, `SHA256SUMS`).
- **Carry (cloud):** workflow `py32-bump.yml` on the default branch (`main`) of
  `patapovich/tuya-zigbee-switch` (fork only) runs hourly, by hand
  (`gh workflow run py32-bump.yml --repo patapovich/tuya-zigbee-switch [-f py32_tag=vX.Y.Z]
  [-f force=true]`) or on a `repository_dispatch` `py32-release`: takes the release's
  `secondary_mcu_image.c` (checksum), refuses an older version than home-dimmer carries
  (unless force), runs lint + tests, commits to `home-dimmer`, rebuilds `home-dimmer_build`
  (= home-dimmer + branch `fork-ci`) and starts `build.yml`, whose "Update firmware files"
  commit has the OTA image. `release.sh` starts it right after the release and waits for both;
  `LOCAL_BUMP=1` does the carrying in `~/projects/tzs-diag` instead.
- **Instant hand-over (optional):** a secret `ZTU_DISPATCH_TOKEN` in zdms16-py32 (fine-grained
  PAT for `patapovich/tuya-zigbee-switch` with Contents: read and write) makes the release job
  send the `py32-release` dispatch itself (then `NO_TRIGGER=1 tools/release/release.sh` only
  waits). Without it the hourly run picks the release up.
- **Deploy (local, home network):** `tools/ha/deploy_ota.sh <ieee> [...]`: newest OTA image
  from `home-dimmer_build`, shows the PY32 version it carries, asks, then fast OTA one module at
  a time with the guard. The ZTU updates the MCU only if it is our firmware and older. Then
  `~/ha-claude/zdms16_health.py <ieee>`.
- By hand (what the scripts do): `make test && make`, `tools/embed_ztu.py build/zdms16_py32.img
  ~/projects/tzs-diag/src/zigbee/secondary_mcu_image.c`, stub tests in tzs-diag, push, build
  branch = home-dimmer + `fork-ci`, `gh workflow run build.yml --ref home-dimmer_build`,
  `zdms16_ota_series.sh` + `zdms16_ota_guard.sh`.

## Converter changes: checklist (after the 2026-11-17 incident)

A template slip commented out the `dimmer_left` light; Home Assistant deleted the left light
of all five modules for 7 minutes. Before deploying any converter change:

1. Dump the exposes with the real zhc (`tools/ha/expose_dump.js`, see `tools/ha/README.md`;
   own MCU and stock MCU versions) and diff the expose set against the deployed one.
2. Check that the other models' definitions are unchanged.
3. Snapshot `/config/.storage/core.entity_registry` before and after the Z2M restart and diff
   the ZDMS16 entities (none removed / added / renamed unless intended).

## Open (for the next session)

1. **Rename endpoints dimmer_left/right → l1/l2** (user's choice). All five modules at once
   (shared converter): ~45 entities get new unique_ids (history restarts), clean up the deleted
   ones, give the new ones the `_l1`/`_l2` ids, update the 6 automation refs
   `number.light_*_on_level_dimmer_left`. Dry run first (expose dump + predicted ids).
2. Long trial of the spare: `zdms16_health.py` now and then (lock losses, resets).
3. Fade / hold defaults: the user set channel 1 to `led_soft`; settle the defaults after use.
4. House-unit migration (not yet wanted): RDP removal (`tools/swd/rdp0_unlock.sh`, mass
   erase) → power cycle → BOR (`pyocd load … --base-address 0x1FFF0E80 tools/swd/ob_bor25.bin`)
   → `make flash-all` → power cycle. Mains off, probe only on a dead board.
5. Optional: zero-cross delay measurement (`docs/ZC-DELAY-MEASUREMENT.md`).
6. Parked: per-channel gesture action modes / detached switch type; module button gestures.
7. Later (user): the release starts the fork's py32-bump at once: a fine-grained PAT for
   `patapovich/tuya-zigbee-switch` (Contents: read and write) as secret `ZTU_DISPATCH_TOKEN` in
   this repo (the user creates and enters it). Until then the hourly run or release.sh does it.

## Safety rules

The board is not isolated from the mains. Never a probe, laptop or earthed scope on it while
it is on mains; mains off and wait a minute before SWD. The SIM build never goes on mains.
