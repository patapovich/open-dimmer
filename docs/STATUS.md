# ZDMS16-2 own PY32 firmware: status (2026-11-18)

Own firmware for the PY32F002A dimming MCU of the AVATTO ZDMS16-2, updated over Zigbee by
the fork's Zigbee (ZTU) firmware (`patapovich/tuya-zigbee-switch`, branch `home-dimmer`).

## Where things are

| What | Version / place |
|---|---|
| Spare module (test lamp on L1, push button L–S1) | ZTU 1.1.3-b2f77929 (0x1103301d), MCU **2.1.10 own**; latest release v2.1.11 (default curve led_soft for unsaved channels only, so not deployed) |
| Other modules | none runs the own MCU firmware yet: see "Before further modules" |
| PY32 source | this repo (github.com/patapovich/zdms16-py32) |
| ZTU + converter | fork `patapovich/tuya-zigbee-switch`, branch `home-dimmer` (carries PY32 v2.1.11); `home-dimmer_build` has the generated converter and the OTA images |
| SWD tools | `tools/swd/install.sh` (one-contact install, docs/INSTALL.md; untested on a stock module yet), the single steps beside it (`.venv` and `pack/` next to the clone, see docs/INSTALL.md) |
| HA / Z2M helpers | `tools/ha/` (see its README): expose dump and configure check with the real zhc, websocket client, OTA deploy |
| Docs | `docs/INSTALL.md`, `docs/REVIEW-2026-11-17.md`, `docs/ZC-DELAY-MEASUREMENT.md`, `docs/stock-firmware-analysis.md` |

## Flash layout and budget

Bootloader 4 KB @0x08000000 (2 136 bytes used), app 12 KB @0x08001000 (header at +0xC0),
settings 4 KB @0x08004000 (32-page ring, record v2 "ZDS2" 40 bytes; v1 "ZDS1" 20 bytes still
read). App 2.1.11: 11 628 bytes (660 bytes free). DIAG / MEASURE builds 11 972 / 11 988 bytes
(health report, raw timing and the pin scan compiled out there; ~300 bytes left).

## DPs (own firmware; stock ones 1..14 unchanged)

| DP | Meaning |
|---|---|
| 1..6 / 7..12 | channel 1 / 2: on, level, min, switch type, max, countdown (stock layout) |
| 14 | power-on, both channels (written: both; reported: channel 1's) |
| 101 / 102 | dimming curve: incandescent, led, led_deep, led_deeper, led_soft (default for new channels), linear |
| 103 / 104 | power-on per channel |
| 105 / 106 | wall switch gesture (momentary type), event: 1 single, 2 double, 3 triple, 4 hold, 5 release |
| 130..134 | health every 2 min: uptime min, reset cause (RCC_CSR >> 24), lock losses, ignored edges, saves |
| 150..154 / 155..159 | per channel: fade on ms, fade off ms, soft dim end %, hold ms, hold dim end % (saved) |
| 110..123 | DIAG / MEASURE builds only (statistics every 10 s) |
| 0xA0..0xA3 | own commands: update notify / block / result, identity |

Min and max are physical conduction limits for every curve (same meaning as the stock MCU).

## Behaviour (as tuned with the lamp)

- Switch on from dark: starts at min, 950 ms to full. Off: 950 ms, the lowest 12 % above min
  slow down to 1 step per 10 ms, 80 ms at min, then dark.
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
  read the health DPs 130..134 of the module (they come every 2 min).
- By hand (what the scripts do): `make test && make`, `tools/embed_ztu.py build/zdms16_py32.img
  <fork>/src/zigbee/secondary_mcu_image.c`, stub tests in the fork, push, build branch =
  home-dimmer + `fork-ci`, `gh workflow run build.yml --ref home-dimmer_build`, then the OTA as
  `deploy_ota.sh` does it.

## Converter changes: checklist (after the 2026-11-17 incident)

A template slip commented out the `dimmer_left` light, and Home Assistant deleted the left
light entity of every module until the fix was deployed. Before deploying any converter change:

1. Dump the exposes with the real zhc (`tools/ha/expose_dump.js`, see `tools/ha/README.md`;
   own MCU and stock MCU versions) and diff the expose set against the deployed one.
2. Check that the other models' definitions are unchanged against the deployed converter
   (against upstream every one of them differs by design: the added `device_config_ext`).
3. Snapshot Home Assistant's entity registry (`core.entity_registry`) before and after the Z2M
   restart and diff the ZDMS16 entities (none removed / added / renamed unless intended).

## Before further modules

The own MCU firmware goes on further modules only through these gates.

**Process rule.** A PY32 version reaches further modules only after the spare has run it and
taken a real ZTU OTA from it (the MCU updated away from that version over Zigbee), so the
version's own update path is proven.

**Hardware gate**, on the spare (none of this has run yet):
- Off the mains: record the PCB marking; check continuity PA0 → Q2 gate network and PB2 → S2
  input stage (against PA1 / PB1 of channel 1); measure the gate–source resistance of Q1 and Q2
  (gate pull-down).
- On the bench: a lamp on L2 and a push button on S2, with every switch type and the gestures;
  both channels at different levels, fading at the same time; a few hours at full and at ~50 %
  load, watching health DPs 131 (reset cause) and 132 (lock losses); 20+ mains cycles and short
  dropouts; an OTA with both channels lit.
- `install.sh`: its re-flash path, then the full stock path (option reload) after restoring the
  spare's saved stock option bytes.
- Tools: measure the CN2 pad pitch (below 2.54 mm, most likely 2.0 mm: ~6.0 mm first to last
  pad centre) and whether the pads are near the board edge; get a matching 4-pin pogo clip
  (2.0 mm PCB clamp fixture) or a 2.0 mm spring-loaded connector + clamp.

**Migration order:**
1. Endpoint rename first (Open 1), while the modules still run the stock MCU, so the new
   per-channel entities of the own firmware are created once and not again at the rename.
2. ZTU first: OTA one module's ZTU to the current build with its stock MCU still in, and watch
   it for a day: `mcu_version` still shows the stock version (no "own"), no expose or entity
   changes, lights, wall switch and OnLevel work. Then the other modules, one at a time. A ZTU
   build from before the own-MCU support has no identity check and no MCU updates: never put
   the own MCU under one.
3. The bootloader changes of the fix plan (phase 3) are released, and the spare runs exactly
   that release's full image (re-flashed with `install.sh`) for a while, including an OTA from it.
4. Then the MCU, one module at a time, as in docs/INSTALL.md: write the settings down, install,
   restore them (curve `linear` first), Reconfigure, check "own", restart Zigbee2MQTT, check the
   entity diff (the shared `power_on_behavior` gone, the per-channel entities added), and watch
   the health DPs for 24–48 h before the next module.

## Review 2026-11-18 and fix plan

A full multi-agent review of both repos was done on 2026-11-18. The report is kept privately
(it contains details of a private installation); its findings turn into commits here and in
the fork. Fix plan, in phases:

0. Now, no hardware: docs, tooling, one-liners (deploy tool, CI gates, version byte rules).
1. ZTU + converter batch: one ZTU release, OTA to the spare first, then one module with its
   stock MCU.
2. PY32 app batch (OTA to the spare; the flash savings first).
3. Bootloader batch (SWD, frozen per unit: most test effort).
4. Hardware verification on the spare (the gate above).
5. Migration of further modules (the order above).

Phases 0 and 1 are in progress.

## Open (for the next session)

1. **Rename endpoints dimmer_left/right → l1/l2** (user's choice). All modules at once (shared
   converter): their entities get new unique_ids (history restarts); clean up the deleted ones,
   give the new ones the `_l1`/`_l2` ids, update the references (automations). Dry run first
   (expose dump + predicted ids). In the same migration: the device-level entities
   `device_config`, `device_config_ext`, `mcu_version`, `multi_press_reset_count` move off
   `dimmer_left` (upstream template puts them on the first endpoint) to the device (no
   endpoint).
1b. ~~Converter configure read the own-MCU settings too early after a module restart~~ done
   2026-11-18 (home-dimmer 8ea5a25e): configure waits for a real `mcu_version` first (checked
   with `tools/ha/configure_check.js`; on hardware at the next OTA).
2. Long trial of the spare: read the health DPs now and then (131 reset cause, 132 lock losses,
   133 ignored edges).
3. Fade / hold defaults: the user set channel 1 to `led_soft`; settle the defaults after use.
4. Further modules: see "Before further modules". Install with
   `FREQ=400000 IMAGE=release tools/swd/install.sh` (one contact; **untested on hardware**, the
   stock path with the option reload has never run) or the single steps in docs/INSTALL.md.
   Mains off, probe only on a dead board.
5. Optional: zero-cross delay measurement (`docs/ZC-DELAY-MEASUREMENT.md`; the DIAG build
   needs a version byte at least the carried one, or the ZTU replaces it within seconds).
6. Parked: per-channel gesture action modes / detached switch type; module button gestures.
7. Later (user): the release starts the fork's py32-bump at once: a fine-grained PAT for
   `patapovich/tuya-zigbee-switch` (Contents: read and write) as secret `ZTU_DISPATCH_TOKEN` in
   this repo (the user creates and enters it). Until then the hourly run or release.sh does it.

## Safety rules

The board is not isolated from the mains. Never a probe, laptop or earthed scope on it while
it is on mains; mains off and wait a minute before SWD. The SIM build never goes on mains.
