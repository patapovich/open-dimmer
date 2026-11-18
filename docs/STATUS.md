# Open Dimmer: status (2026-11-18)

Open Dimmer is open-source firmware for the PY32F002A dimming MCU of the AVATTO ZDMS16-2,
updated over Zigbee by the fork's Zigbee (ZTU) firmware (`patapovich/tuya-zigbee-switch`,
branch `home-dimmer`).

## Where things are

| What | Version / place |
|---|---|
| Spare module (test lamp on L1, push button L–S1) | ZTU 1.1.3-ed1016b5 (0x1103304d, carries PY32 v2.1.12), MCU **Open Dimmer 2.1.12** since 2026-11-18 (phase 2: updated by the ZTU over the UART, settings kept, health clean) |
| Other modules | none runs Open Dimmer yet: see "Before further modules" |
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

App 2.1.12 (phase 2 of the fix plan below): 11 472 bytes (816 bytes free); DIAG / MEASURE
11 364 / 11 376 bytes (~900 left: they decline every update, so the update path is compiled out
there); SIM 11 692 bytes. The flash savings paid for the phase 2 fixes: 928 bytes less in every
build with the same behaviour (own byte-loop mem* in src/libc_min.c instead of newlib's, no
signed division code). CI warns when a build has under 512 bytes free. The bootloader is
unchanged (2 136 bytes, byte-identical to the released one).

Review 2026-11-18 items done in phase 2: 7 (the MCU's start burst check), 12 and 25 (zero-cross
windows, phase.c on the PC), 13 (watchdog and fault restore), 14, 17, 29, 30, 32 (UART above
the zero cross), 33, 34, 35, 36, 37, 38, 39, 44 (host harnesses for the store, flash driver,
protocol, UART, health, dimmer.c and controls.c), 50, and parts of 18 (the SIM build's guard:
PA4 pull-up and EXTI, shut down from the ISR, virtual edges only after PA4 is static; the app
declines 0xBF / 0xFF offers), 40 (flash SR flags in the app, the bootloader takes them in phase
3; switch_type clamped on load) and 59 (the savings above, the CI warning). Still open of
those: 18 `make flash` refusing SIM images unless ALLOW_SIM=1, the note that a unit stuck on SIM
needs a 3.x image or SWD; 40 the 60 Hz comment, the INSTALL line that Open Dimmer needs the
fork's ZTU; 59 register-level UART and clock init. ZTU side: also refuse the DIAG header flag
0x04 (tools/embed_ztu.py already does).

## DPs (Open Dimmer; stock ones 1..14 unchanged)

| DP | Meaning |
|---|---|
| 1..6 / 7..12 | channel 1 / 2: on, level, min, switch type, max, countdown (stock layout) |
| 14 | power-on, both channels (written: both; reported: channel 1's) |
| 101 / 102 | dimming curve: incandescent, led, led_deep, led_deeper, led_soft (default for new channels), linear |
| 103 / 104 | power-on per channel |
| 105 / 106 | wall switch gesture (momentary type), event: 1 single, 2 double, 3 triple, 4 hold, 5 release |
| 130..135 | health every 2 min: uptime min, reset cause (RCC_CSR >> 24), lock losses, ignored edges, saves, UART receive losses |
| 150..154 / 155..159 | per channel: fade on ms, fade off ms, soft dim end %, hold ms, hold dim end % (saved) |
| 110..123 | DIAG / MEASURE builds only (statistics every 10 s) |
| 0xA0..0xA3 | Open Dimmer commands (not Tuya's): update notify / block / result, identity |

Min and max are physical conduction limits for every curve (same meaning as the stock MCU).

## MCU identity in Zigbee2MQTT (for developers)

The ZTU asks the MCU who it is (identity command 0xA3; only Open Dimmer answers). Basic
attribute 0xff12 on endpoint 1 then carries `"x.y.z own"` for an MCU identified as Open Dimmer
and the bare version otherwise: for a stock MCU (`"1.0.7"`), and for Open Dimmer when its
identity answer was lost (the ZTU asks once more first, REVIEW M9). `own` is the ZTU's
internal marker: it stays on the wire, and the converter's detection (the `auto` mode of
`open_dimmer_settings`, the configure checks) works on this raw value. Users see two fields
instead:

| Basic 0xff12 | `mcu_version` | `mcu_firmware` |
|---|---|---|
| `"2.1.10 own"` | `2.1.10` | `Open Dimmer` |
| `"1.0.7"` | `1.0.7` | `Tuya (stock)` |
| `"2.1.10"` (identity answer lost) | `2.1.10` | `Open Dimmer` if an `own` value was seen before (kept in device.meta), else `Tuya (stock)` |
| `""` or `"0.0.0"` (right after a restart, before the MCU has told the ZTU) | as reported | not published (the last value stays) |

The converter keeps the last real version in device.meta, and Open Dimmer is sticky there: a
bare 2.x or later does not replace a kept `"x.y.z own"`. Only a version below 2.x means the MCU
really changed (a stock MCU) and replaces it. The `auto` mode of `open_dimmer_settings` follows
the same rule, so the settings stay shown when an identity answer is lost. `mcu_firmware` is a
device-level diagnostic text (no endpoint), only on boards whose device_db entry has
`secondary_mcu_image` (the ZDMS16-2). The device option `open_dimmer_settings` (auto / show /
hide) was called `own_mcu_settings`; the old key is still read when the new one is not set.
Converters from before this rename show the raw value in `mcu_version` (`2.1.10 own`) and have
no `mcu_firmware`; the user docs (INSTALL.md) say so where they check `mcu_firmware`.

## Behaviour (as tuned with the lamp)

- Switch on from dark: starts at min, 950 ms to full. Off: 950 ms, the lowest 12 % above min
  slow down to 1 step per 10 ms, 80 ms at min, then dark.
- Momentary switch: tap from dark → quickly to the stored level; hold from dark → rises
  slowly from min (pick a very dim level); hold while lit → full rate up (no slow start),
  slow near min going down; release stops at once. The first hold after a start goes
  towards the far end (down above the middle of min..max), later ones alternate. The level
  a hold picks is saved. A hold ends by itself after 2 × hold ms + 2 s (a stuck or shorted
  input gives the network its levels back). A network on to a lit channel does not end a
  hold (the ZTU writes DP 1 = 1 before every level change); an off does, also the module
  button's.
- Module button: both channels (any on → all off). ≥ 3 s: rejoin.
- An input closed at power-up rests there: its release is no gesture, no button toggle and
  no rejoin (a toggle or state switch takes it as a real change). A switch type change to
  state sets the light from the switch at once.
- After a watchdog, fault or update restart the light comes back as it was (on/off and
  level), not by the power-on setting; once in a row only: another such reset within a
  minute, and every power cut or brown-out, takes the power-on setting. A fault restarts at
  once (no ~1 s dark wait for the watchdog). Host tests only, **untested on hardware**: the
  watchdog and fault restore, the reset-flag decode and the fault reset (the update restore
  of earlier versions has run on the spare); see the hardware gate.
- A min or max write keeps the level inside min..max (the ack reports the level with it).
- 50 and 60 Hz: the zero-cross tracker takes 40..70 Hz; fade and hold rates scale with the
  measured half period (1/64 steps per 10 ms), so the times in ms hold on both (tested).
- The board's S1 input stays low ~0.1 s after release: a fast double tap merges in
  hardware; doubles need ~0.2 s gaps.

## Build and deploy (CI / CD)

- **CI** (`.github/workflows/ci.yml`, every push / PR, read-only token): host tests (C logic,
  the settings store, the flash driver against a register model, the serial protocol, UART
  receive, health report, the MCU's start burst size, and the image tools); normal, DIAG,
  MEASURE and SIM builds with the pinned SDK; curve tables vs generator; ZTU embedding refuses
  bench, measurement and diagnostics builds; size report; artifacts.
- **Release:** bump `FW_VERSION` in `src/tuya.h` (never 0xBF, reserved for SIM, or 0xFF; a
  warning comes when fewer than 16 codes are left), add the version's short section to
  `CHANGELOG.md` (the release notes; `release.sh` and the release job refuse a version without
  one), commit on `main`,
  `tools/release/release.sh`: checks (main, clean, contains origin/main, tests, all four
  builds), tags `vX.Y.Z` (must match the image); the tag's CI run publishes the release only
  after the tests and builds passed (image, full image, bootloader, `secondary_mcu_image.c`,
  `SHA256SUMS`: plain file names, `sha256sum -c --ignore-missing SHA256SUMS` checks a
  download). Running `release.sh` again resumes (re-runs a failed tag run; a release counts
  only when the run's tests, builds and release job passed). A re-run tests the tag's commit,
  so once HEAD has changed since an unreleased tag (beyond docs), `release.sh` stops and says
  how to move the tag to HEAD instead.
- **Carry (cloud):** workflow `py32-bump.yml` on the default branch (`main`) of
  `patapovich/tuya-zigbee-switch` (fork only) runs hourly, by hand
  (`gh workflow run py32-bump.yml --repo patapovich/tuya-zigbee-switch [-f py32_tag=vX.Y.Z]
  [-f force=true]`) or on a `repository_dispatch` `py32-release`: takes the release's
  `secondary_mcu_image.c` (checksum), refuses an older version than home-dimmer carries or a
  different image of the same version (unless force; the hourly run only notes it), runs
  lint + tests, commits to `home-dimmer`, rebuilds `home-dimmer_build`
  (= home-dimmer + branch `fork-ci`) and starts `build.yml`, whose "Update firmware files"
  commit has the OTA image. `release.sh` starts it right after the release and waits for both;
  `LOCAL_BUMP=1` does the carrying in `~/projects/tzs-diag` instead.
- **Instant hand-over (set up 2026-11-18, tested):** a secret `ZTU_DISPATCH_TOKEN` in zdms16-py32 (fine-grained
  PAT for `patapovich/tuya-zigbee-switch` with Actions: read and write only, no Contents
  write) makes the release's hand-over job start py32-bump itself (then
  `NO_TRIGGER=1 tools/release/release.sh` only waits). A failing hand-over (expired token) does
  not fail the run: `release.sh` warns and starts py32-bump itself. Without the token the
  hourly check picks the release up (best effort: GitHub may delay scheduled runs and disables
  them after 60 days without activity; `release.sh` enables it again).
- **Deploy (local, home network):** `tools/ha/deploy_ota.sh <ieee | friendly_name:ieee>...`
  with the local OTA series and guard helpers in `$HA_HELPERS` (settings: `tools/ha/README.md`):
  newest OTA image from `home-dimmer_build`, refused unless it carries a valid PY32 image; shows
  the PY32 version it carries and each module's installed vs the image's file version, asks
  (once more if the image is not newer), then fast OTA one module at a time with the guard; the
  250 ms block delay is put back on every exit. The ZTU updates the MCU only if it runs an
  older version of Open Dimmer. Then read the health DPs 130..135 of the module (they come
  every 2 min).
- By hand (what the scripts do): `make test && make`, `tools/embed_ztu.py build/zdms16_py32.img
  <fork>/src/zigbee/secondary_mcu_image.c`, stub tests in the fork, push, build branch =
  home-dimmer + `fork-ci`, `gh workflow run build.yml --ref home-dimmer_build`, then the OTA as
  `deploy_ota.sh` does it.

## Converter changes: checklist (after the 2026-11-17 incident)

A template slip commented out the `dimmer_left` light, and Home Assistant deleted the left
light entity of every module until the fix was deployed. Before deploying any converter change:

1. Dump the exposes with the real zhc (`tools/ha/expose_dump.js`, see `tools/ha/README.md`;
   with the raw MCU versions of Open Dimmer, `"2.1.10 own"`, of the stock MCU, `"1.0.7"`, and
   of an MCU not known yet, `""`) and diff the expose set against the deployed one.
2. Check that the other models' definitions are unchanged against the deployed converter
   (against upstream every one of them differs by design: the added `device_config_ext`).
3. Snapshot Home Assistant's entity registry (`core.entity_registry`) before and after the Z2M
   restart and diff the ZDMS16 entities (none removed / added / renamed unless intended).

## Before further modules

Open Dimmer goes on further modules only through these gates.

**Process rule.** A PY32 version reaches further modules only after the spare has run it and
taken a real ZTU OTA from it (the MCU updated away from that version over Zigbee), so the
version's own update path is proven.

**Hardware gate**, on the spare:
- Done 2026-11-18: a lamp on L2 and a push button on S2 work (momentary: on, off, hold
  dimming, gestures single / hold / release in the MCU log), so PA0 drives the channel 2 gate
  and PB2 reads S2 as the code assumes.
- Off the mains: record the PCB marking; measure the gate–source resistance of Q1 and Q2 (gate
  pull-down).
- On the bench: S2 with the toggle and state switch types;
  both channels at different levels, fading at the same time; a few hours at full and at ~50 %
  load, watching health DPs 131 (reset cause) and 132 (lock losses); 20+ mains cycles and short
  dropouts; an OTA with both channels lit.
- Reset restore (the first version with it): a forced watchdog reset and a forced HardFault
  each bring the light back as it was (health DP 131 shows IWDG or software), a second one
  within a minute takes the power-on setting, a power cut or a brown-out takes the power-on
  setting, and an update from it still restores the light.
- `install.sh`: its re-flash path, then the full stock path (option reload) after restoring the
  spare's saved stock option bytes.
- Tools: measure the CN2 pad pitch (below 2.54 mm, most likely 2.0 mm: ~6.0 mm first to last
  pad centre) and whether the pads are near the board edge; get a matching 4-pin pogo clip
  (2.0 mm PCB clamp fixture) or a 2.0 mm spring-loaded connector + clamp.

**Migration order:**
1. Endpoint rename first (Open 1), while the modules still run the stock MCU, so the new
   per-channel entities of Open Dimmer are created once and not again at the rename.
2. ZTU first: OTA one module's ZTU to the current build with its stock MCU still in, and watch
   it for a day: `mcu_firmware` still shows "Tuya (stock)" and `mcu_version` the stock version,
   no expose or entity changes, lights, wall switch and OnLevel work. Then the other modules,
   one at a time. A ZTU build from before the Open Dimmer support has no identity check and no
   MCU updates: never put an Open Dimmer MCU under one.
3. The bootloader changes of the fix plan (phase 3) are released, and the spare runs exactly
   that release's full image (re-flashed with `install.sh`) for a while, including an OTA from it.
4. Then the MCU, one module at a time, as in docs/INSTALL.md: write the settings down, install,
   restore them (curve `linear` first), Reconfigure, check that `mcu_firmware` shows
   "Open Dimmer", restart Zigbee2MQTT, check the entity diff (the shared `power_on_behavior`
   gone, the per-channel entities added), and watch the health DPs for 24–48 h before the next
   module.

## Review 2026-11-18 and fix plan

A full multi-agent review of both repos was done on 2026-11-18. The report is kept privately
(it contains details of a private installation); its findings turn into commits here and in
the fork. Fix plan, in phases:

0. Now, no hardware: docs, tooling, one-liners (deploy tool, CI gates, version byte rules).
1. ZTU + converter batch: one ZTU release, OTA to the spare first, then one module with its
   stock MCU.
2. PY32 app batch (OTA to the spare; the flash savings first).
3. Bootloader batch (SWD, frozen per unit: most test effort). It also takes the flash driver's
   SR-flag fix of phase 2 (item 40): `src/flash_drv.c` keeps the bootloader's old behaviour
   under `BOOTLOADER`, so `boot.bin` (what every `make flash-all` and `install.sh` writes) stays
   byte-identical to the one before phase 2 until this batch drops that and tests it on the
   spare. Flash layout decided for it (2026-11-18): the bootloader stays in its own 4 KB
   sector (2 136 B now, ~2.3–2.4 KB with the rescue paths; the sector stays write-protectable);
   it accepts an app of up to ~14 KB (its own limit, not the app's `APP_MAX_SIZE`), so the app
   can later grow into half of the settings area; the settings ring may shrink from 4 KB to
   2 KB (32 → 16 pages, the app moves its records itself); the app checks an update offer
   against its own `_settings_start`, so an image can never overwrite the settings.
4. Hardware verification on the spare (the gate above).
5. Migration of further modules (the order above).

Phase 0 is done (2026-11-18). Phase 1 is released as ZTU 1.1.3-194952ee (fork home-dimmer
aa5f02e4, build branch 194952ee) and the converter from it: on the spare since 2026-11-18 (MCU
took 2.1.11 by itself; On to a lit channel keeps its level, OnLevel from off, gestures
change-only after a Reconfigure) and on the further modules with their stock
MCUs (2026-11-18, one at a time; no entity change). OnLevel kept across a power cycle
(checked on the spare). Phase 2 is released as Open Dimmer 2.1.12 (on the spare).

## Open (for the next session)

1. **Rename endpoints dimmer_left/right → l1/l2** (user's choice). All modules at once (shared
   converter): their entities get new unique_ids (history restarts); clean up the deleted ones,
   give the new ones the `_l1`/`_l2` ids, update the references (automations). Dry run first
   (expose dump + predicted ids). In the same migration: the device-level entities
   `device_config`, `device_config_ext`, `mcu_version`, `multi_press_reset_count` move off
   `dimmer_left` (upstream template puts them on the first endpoint) to the device (no
   endpoint), where `mcu_firmware` already is.
1b. ~~Converter configure read the Open Dimmer settings too early after a module restart~~ done
   2026-11-18 (home-dimmer 8ea5a25e): configure waits for a real `mcu_version` first (checked
   with `tools/ha/configure_check.js`; on hardware at the next OTA).
2. Long trial of the spare: read the health DPs now and then (131 reset cause, 132 lock losses,
   133 ignored edges, 135 UART receive losses).
3. Fade / hold defaults: the user set channel 1 to `led_soft`; settle the defaults after use.
4. Further modules: see "Before further modules". Install with
   `FREQ=400000 IMAGE=release tools/swd/install.sh` (one contact; **untested on hardware**, the
   stock path with the option reload has never run) or the single steps in docs/INSTALL.md.
   Mains off, probe only on a dead board.
5. Optional: zero-cross delay measurement (`docs/ZC-DELAY-MEASUREMENT.md`; the DIAG build
   declines the ZTU's update offers, so it stays until `make flash-all` over SWD).
6. Parked: per-channel gesture action modes / detached switch type; module button gestures.
7. Done 2026-11-18: `ZTU_DISPATCH_TOKEN` is set; a re-run of v2.1.12's hand-over job started
   py32-bump in the fork, which saw 2.1.12 already carried and changed nothing. Releases now use
   `NO_TRIGGER=1 tools/release/release.sh`. Renew the token before it expires (a failing
   hand-over does not fail the release; `release.sh` without `NO_TRIGGER` still works).

## Safety rules

The board is not isolated from the mains. Never a probe, laptop or earthed scope on it while
it is on mains; mains off and wait a minute before SWD. The SIM build never goes on mains.
