# Open Dimmer: status (2026-11-20, release 2.2.4)

Open Dimmer is open-source firmware for the PY32F002A dimming MCU of the AVATTO ZDMS16-2,
updated over Zigbee by the fork's Zigbee (ZTU) firmware (`patapovich/tuya-zigbee-switch`,
branch `home-dimmer`).

## Where things are

| What | Version / place |
|---|---|
| Spare module (test lamp on L1, push button L–S1) | ZTU 1.1.3-6598b921 (carries PY32 v2.2.2), MCU **Open Dimmer 2.2.2** on the **v2 bootloader** (SWD, boot sector CRC 0x7D215316), settings ring moved. Bench tests done: B1; B2 (probe answer 172..183 ms, also with a carried image over 12 KB thanks to the gate's refusal answer); **B7** (mains cut in the middle of the 2.2.0 -> 2.2.1 update: the next power-up had no app, the bootloader asked in mode 2, the ZTU served it past the ring gate, 2.2.1 ran 4 s after power-up, settings kept); DP 138 at every report from 2.2.2 (19..20 degC at room temperature). Transitions checked from Home Assistant (none = the module fade, 0 = instant, brightness_fade_time proportional) |
| Other modules | none runs Open Dimmer yet: see "Before further modules" |
| PY32 source | this repo (github.com/patapovich/zdms16-py32) |
| ZTU + converter | fork `patapovich/tuya-zigbee-switch`, branch `home-dimmer` (carries PY32 v2.2.2; dimmer-only build; ring gate with a refusal answer to gated probes; transitions: no time = the module fade, 0 = instant, per-channel brightness_fade_time; build changelog in `docs/changelog_home_dimmer.md`); `home-dimmer_build` has the generated converter and the OTA images |
| SWD tools | `tools/swd/install.sh` (one-contact install, docs/INSTALL-PROBE.md; untested on a stock module yet), the single steps beside it (`.venv` and `pack/` next to the clone, see docs/INSTALL-PROBE.md) |
| HA / Z2M helpers | `tools/ha/` (see its README): expose dump and configure check with the real zhc, websocket client, OTA deploy |
| Docs | `docs/INSTALL.md` (two wires), `docs/INSTALL-PROBE.md` (probe), `docs/REVIEW-2026-11-17.md`, `docs/ZC-DELAY-MEASUREMENT.md`, `docs/stock-firmware-analysis.md` |

## Bench tests (spare, 2026-11-19; docs/BOOTLOADER.md bench table)

On probe power (mains off; CN2 VCC feeds the Zigbee module too, so it serves its image over the
UART), with the ZTU's ring gate letting bootloader requests through when the installed app has
RING_HIGH:
- B1, B2 (probe answer 172..184 ms, also with a carried image over 12 KB: the gate's refusal
  answer ends the probe at once), B7 once (mains cut in the middle of an update: mode-2
  recovery, running 4 s after the power-up; 2 more cuts with the next release).
- B3: a build declining every offer (`make OFFER_DECLINE_TEST=1 FW_PREV=1 FW_TEST=1`) declined
  it; after a power cut the probe installed the release (182 ms, WRITTEN).
- B4: a build faulting 1 s after start (`make CRASH_TEST=1 FW_TEST=1`): RESCUE + WRITTEN, the
  release back within seconds.
- B5: button through power-up: same image → BUTTON, nothing written, the app after the 30 s
  window; held 8 s → STUCK, nothing written, no rejoin; different image → BUTTON + WRITTEN.
- B8: DP 131 = PWR only after a power cycle that follows a software reset; the software flag
  survives the bootloader (its LSION write keeps the flags); the IWDG does not survive
  `NVIC_SystemReset` (RCC_CSR 0x10000000 with LSI off; no watchdog reset in 3 s halted with the
  freeze off).
- B11 (`make LINK_WDG_TEST=1 FW_PREV=1 FW_TEST=2`): a channel on for 9+ min → no restart; both
  off → a restart ~1 min later, LINK + WRITTEN, link probe 8 ms, the release installed. With the
  test build carried itself (a local Zigbee build, `tools/ztu_local_build.sh`): 10 link restarts in
  a row, LINK, probe answer 7–10 ms (nothing newer: the app at once), never a rescue window.
- B9: an offered update (2.2.2 → 2.2.3) with both channels lit: both came back at their levels.
  The dark time was not caught (the Zigbee module's 30-entry MCU log had moved on).
- `install.sh` with an image over 12 KB kept the settings on the hardware (sector 4 merged).
- B6 dropped: no unit runs a 2.1.x app under the v2 bootloader (v1 was never distributed; every
  install is a full image of one generation). 2 more B7 cuts when a switchable plug is at hand; B10 trial running.

## Flash layout and budget

From 2.1.14 (release M, build 400) on: app up to 14 KB @0x08001000 under the v2 bootloader,
settings 2 KB @0x08004800 (16-page ring, the sector's upper half; header flag RING_HIGH 0x10).
2.1.14 moves the newest record up once (docs/BOOTLOADER.md section 1); apps over 12 KB need the
v2 bootloader and a moved ring (the ZTU's ring gate: DP 137 bit 25 seen). The app is built with
LTO (the bootloader is not, it stays byte-identical). App 2.1.14: 11 652 bytes. App 2.2.0:
13 268 bytes; 2.2.2: 13 288 bytes (1 048 free of 14 KB).

History:


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

Phase 3 (bootloader v2, release R = 2.1.13, build 300; integrated, not released yet): app
11 632 bytes (656 bytes free of 12 KB; first 12 120 with 168 free, then -488 with the same
behaviour: clock and UART from constants, item 59 below, and the app's data in common
sections); DIAG / MEASURE / SIM 11 264 / 11 268 / 11 320 (the link watchdog and the
confirmation are compiled out of the bench builds). Bootloader v2 3 364 of 4 096 bytes (732
free; the design's prototype 3 252; +8 for the watchdog reload after its update wait), boot
sector CRC-32 0x7D215316 for this build (the release notes give the released value). The Zigbee
firmware's OTA file with this app carried (phase-3 ZTU, ZDMS16-2 build): 211 426 bytes, 1 566
below the 0x34000 limit; a 14 KB app (after the ring move) would need 1 138 more, so the ZTU
build must free room first (docs/BOOTLOADER.md section 12). `release.sh` prints the app
headroom and this margin together and refuses below 512 free app bytes unless
`ACCEPT_HEADROOM=<the free bytes>` records the user's decision.

Review 2026-11-18 items done in phase 2: 7 (the MCU's start burst check), 12 and 25 (zero-cross
windows, phase.c on the PC), 13 (watchdog and fault restore), 14, 17, 29, 30, 32 (UART above
the zero cross), 33, 34, 35, 36, 37, 38, 39, 44 (host harnesses for the store, flash driver,
protocol, UART, health, dimmer.c and controls.c), 50, and parts of 18 (the SIM build's guard:
PA4 pull-up and EXTI, shut down from the ISR, virtual edges only after PA4 is static; the app
declines 0xBF / 0xFF offers), 40 (flash SR flags in the app, the bootloader takes them in phase
3; switch_type clamped on load) and 59 (the savings above, the CI warning). Still open of
those: 18 `make flash` refusing SIM images unless ALLOW_SIM=1, the note that a unit stuck on SIM
needs a 3.x image or SWD; 40 the 60 Hz comment, the INSTALL line that Open Dimmer needs the
fork's ZTU. 59's register-level UART and clock init came in phase 3 (clock and baud rate from
constants, see above). ZTU side: also refuse the DIAG header flag 0x04 (tools/embed_ztu.py
already does).

## DPs (Open Dimmer; stock ones 1..14 unchanged)

| DP | Meaning |
|---|---|
| 1..6 / 7..12 | channel 1 / 2: on, level, min, switch type, max, countdown (stock layout; from 2.2.0 the countdown is an auto-off in s, up to a day, RAM only, reported 0 when an off ends it; from 2.2.4 a wall switch hold reports the level every 0.5 s on the way, as the stock MCU) |
| 14 | power-on, both channels (written: both; reported: channel 1's) |
| 101 / 102 | dimming curve: incandescent, led, led_deep, led_deeper, led_soft (default for new channels), linear |
| 103 / 104 | power-on per channel |
| 105 / 106 | wall switch gesture (momentary type), event: 1 single, 2 double, 3 triple, 4 hold, 5 release, 6 press (fast press, 2.2.0) |
| 130..135 | health every 2 min: uptime min, reset cause (RCC_CSR >> 24), lock losses, ignored edges, saves, UART receive losses |
| 136 / 137 | with the health report (from the first v2 app): CRC-32 of the 4 KB boot sector (the released v1 bootloader: 0xEAC45031), the boot word at start, bits 23..0 (probe answer time, rescue info, LINK, tries; 0 under v1). docs/BOOTLOADER.md section 9. From 2.1.14 bit 24: the settings ring is the sector's upper half (RING_HIGH), bit 25: the newest settings record is in it ("ring moved") |
| 150..154 / 155..159 | per channel: fade on ms, fade off ms, soft dim end %, hold ms, hold dim end % (saved) |
| 138 / 139 | (2.2.0) die temperature degC (signed; VDD dependent, about +-10 degC absolute, a diagnostic) / the chip's sensor calibration (raw 85 degC << 16 \| raw 30 degC) |
| 162 / 163 | (2.2.0, saved) wall switch mode: 0 normal, 1 detached (only reports), 2 off |
| 164 / 165 | (2.2.0, saved) level after a power cut (0: the last level; kept in steps of 4) |
| 166 / 167 | (2.2.0, not saved) identify: blink for that many s (up to 255), 0 stops |
| 168 / 169 | (2.2.0, saved) start boost ms (steps of 50, up to 750, 0 none) |
| 170 / 171 | (2.2.0, saved) fast press: in detached mode a momentary press is reported at once (6) |
| 160 / 161 | per channel (from build 400): transition time in ms for the network changes of the next second (a Zigbee transition time; one even rate, the min when dark; 0 = the fade settings). Not saved, not in the dump; the module writes it right before the on / off / level |
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
  receive, health report, the MCU's start burst size, the image, install and release tools, the
  build number rules, and `make test-boot`: the bootloader against its models); normal, DIAG,
  MEASURE and SIM builds with the pinned SDK; the bootloader's layout (`make check-boot`,
  `tools/check_elf.py`) and each image's header flags and build (`tools/check_app.py`); curve
  tables vs generator; ZTU embedding refuses bench, measurement and diagnostics builds; size
  report (with the bootloader and its boot sector CRC); artifacts.
- **Release:** bump `FW_VERSION` in `src/tuya.h` (never 0xBF, reserved for SIM, or 0xFF; a
  warning comes when fewer than 16 codes are left) and `FW_BUILD` next to it (the previous
  release tag's + 100, the first one 300; spare test builds are `make FW_TEST=1..99`, below the
  release `make FW_PREV=1 FW_TEST=n`, bench test B11's `LINK_WDG_TEST=1` only so; the version
  byte stays; docs/BOOTLOADER.md section 2), add the version's short section to
  `CHANGELOG.md` (the release notes; `release.sh` and the release job refuse a version without
  one), commit on `main`,
  `tools/release/release.sh`: checks (main, clean, contains origin/main, tests, all four
  builds; `tools/release/release_check.py`: the build number, the version byte above every other
  release tag, the ring-mover gate for the first release over 12 KB, the app headroom and the
  ZTU OTA margin together (`ACCEPT_HEADROOM`, `ZTU_OTA`, see above); it prints the bootloader's
  boot sector CRC, which the release notes also carry), tags `vX.Y.Z` (must match the image);
  the tag's CI run publishes the release only
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
  commit has the OTA image. It then stamps the fork's build changelog (home-dimmer
  `docs/changelog_home_dimmer.md`: its "Next" becomes the section of that build, named by the
  `swBuildId` Z2M shows); new ZTU changes go under "Next" with their commit, and
  `tools/ha/deploy_ota.sh` shows the section of the build it deploys. `release.sh` starts it
  right after the release and waits for both;
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
   MCU updates, and the stock Tuya ZTU none either (Open Dimmer under it is not supported, but
   safe: docs/BOOTLOADER.md section 15): never put an Open Dimmer MCU under one.
   Per module, also a UART-level check from the fork's link diagnostics (counters 0xff40, log
   0xff41..0xff45, `helper_scripts/decode_mcu_diag.py`), decoded and saved before and after its
   OTA: `mcu_version` reads the stock version before; after it, TX_CMD entries only 0x0B, 0x28
   and the expected DP writes (no 0xA0..0xA3), RX_DP for DPs 3, 4, 5 and 14 after the first
   contact, the min / max entities as before, `rx_bad` and `tx_timeout` not growing over a day.
   Then one ZTU restart with the light on (a config re-write): light state and level unchanged.
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
3. Bootloader batch (SWD, frozen per unit: most test effort): bootloader v2 as designed in
   docs/BOOTLOADER.md (alone in its 4 KB sector, apps up to 14 KB, power-on probe, rescue by a
   try counter and by the module button, the link-watchdog hook; the flash driver shared with
   the app without the old `BOOTLOADER` shim), the app side (build numbers, confirmation, the
   link watchdog, DP 136 / 137), the tools (image rules, `install.sh`, release checks) and the
   phase-3 ZTU in the fork. State below ("Phase 3").
4. Hardware verification on the spare (the gate above).
5. Migration of further modules (the order above).

Phase 0 is done (2026-11-18). Phase 1 is released as ZTU 1.1.3-194952ee (fork home-dimmer
aa5f02e4, build branch 194952ee) and the converter from it: on the spare since 2026-11-18 (MCU
took 2.1.11 by itself; On to a lit channel keeps its level, OnLevel from off, gestures
change-only after a Reconfigure) and on the further modules with their stock
MCUs (2026-11-18, one at a time; no entity change). OnLevel kept across a power cycle
(checked on the spare). Phase 2 is released as Open Dimmer 2.1.12 (on the spare).

## Phase 3 (bootloader v2)

Implemented and integrated, tested on the PC only; nothing of it has run on hardware.

- Host tests: `make test` (with `make test-boot`, the real `boot/boot.c` against flash, RAM,
  UART, watchdog and ZTU models: probe, rescue, button, link probe, power cuts at every page,
  noise, fuzz, stock Tuya ZTU), the app's offer, identity, confirmation and link watchdog tests,
  the image and release tools. The fork's stub tests of the phase-3 ZTU pass with release R's
  app carried, including the factory MCU gate (the UART towards a stock MCU equals phase 1's).
- Co-simulation (not in the repo): the real v2 and v1 bootloaders and the real apps (R, test
  builds, 2.1.12) against the phase-3 ZTU stub: updates from 2.1.12 under v1 and v2, offers by
  build, probe, counter and button rescue, the link watchdog (with its times cut), the stock
  Tuya ZTU model, and a fuzz of 200 seeds (Zigbee commands, wall switch, resets, power cuts,
  ZTU restarts with other images): no difference between the ZTU's and the MCU's state, no
  bootloader rule broken.

Still to do, in this order (docs/BOOTLOADER.md section 14):
1. Done 2026-11-19: release R = v2.1.13 (build 300, boot sector CRC of its v2 bootloader
   0x7D215316), with `ZTU_OTA` set to a local build of the phase-3 ZTU (1530 bytes below
   0x34000 with R carried).
2. Phase-3 ZTU 1.1.3-db24e888 carrying R: on the spare 2026-11-19 (the spare took R under v1
   by the version byte). Still to do: the other modules' ZTUs one at a time with their stock
   MCUs, each with the per-module check of migration step 2.
3. Spare: `IMAGE=<R's zdms16_py32_full.bin> tools/swd/install.sh` (also the first run of the
   re-flash path), then the bench tests B1..B11 of docs/BOOTLOADER.md section 14 on the spare,
   with the mains rules (the case closed on mains, the button pressed through it). None has run
   yet. B8 (the reset flags) decides whether the cold / warm reading of the bootloader holds on
   the chip; B2 checks the ZTU's 1.5 s answer time; B10 is the 1-2 week trial.
4. Only then further modules (the migration order above).
5. Release M = 2.1.14 (build 400, the ring mover of docs/BOOTLOADER.md section 1, at most
   12 KB so the v1 bootloader runs it too): the ring moves to the upper half; check on every
   Open Dimmer module that DP 137 bit 25 ("ring moved") is set before any app over 12 KB.
   Then, with the v2 bootloader on the module and a ZTU build with room, 2.2.0 (over 12 KB).

**Per ZTU release (contract check).** The frozen bootloader relies on the ZTU contract
(docs/BOOTLOADER.md section 10, `src/ota_proto.h`): every ZTU release that carries an image runs
the fork's contract tests, and on the spare B2 (20+ power cycles: DP 137 probe time at most
1500 ms each, the light at probe time + ~50 ms) right after its OTA. A ZTU release that misses
the contract is fixed before it goes to any further module; the bootloader's 3 s window stays.

**DP 136 / 137 (health report, every 2 min).** DP 136 must equal the boot sector CRC of the
release the module was installed with (the released v1: 0xEAC45031); anything else means the
bootloader sector changed. DP 137 is the boot word at the start of the run: bits 3..0 the tries
(1 at every start under a working ZTU, before the run confirmed), bit 6 LINK (a link-watchdog
restart since the power-up: should stay clear under a working ZTU), bits 11..8 RESCUE, BUTTON,
WRITTEN, STUCK since the power-up, bits 23..12 the probe's answer time in ms (0: no answer). 0
under the v1 bootloader. Watch for RESCUE, LINK or STUCK, a probe time above 1500 ms, and DP 131
resets that are not explained.

## Open (for the next session)

1. ~~Rename endpoints dimmer_left/right → l1/l2~~ done 2026-11-19 (fork home-dimmer 88d63066,
   PR #558 too): channels `l1` / `l2` as zigbee-herdsman-converters' stock ZDMS16-2 names them
   (a module moved from the stock firmware keeps its light and switch type entities);
   `mcu_version`, `multi_press_reset_count`, `device_config`, `device_config_ext` on the device.
   Same change: Open Dimmer times in seconds (`fade_on_time`, `fade_off_time`, `hold_time`,
   `start_boost`; the MCU keeps ms), `soft_dim_end` / `hold_dim_end` → `fade_off_slow_zone` /
   `hold_slow_zone`, labels (Wall switch type / mode / fast press, On level (wall switch),
   Power-on level), `mcu_temperature_calibration` not exposed. Home Assistant: new unique_ids;
   rename the old entities to the new ids before the switch (history follows the entity id),
   remove the old entries while Z2M is stopped, copy names / hidden / Assist exposure.
1b. ~~Converter configure read the Open Dimmer settings too early after a module restart~~ done
   2026-11-18 (home-dimmer 8ea5a25e): configure waits for a real `mcu_version` first (checked
   with `tools/ha/configure_check.js`; on hardware at the next OTA).
2. Long trial of the spare: read the health DPs now and then (131 reset cause, 132 lock losses,
   133 ignored edges, 135 UART receive losses).
3. Fade / hold defaults: the user set channel 1 to `led_soft`; settle the defaults after use.
4. Further modules: see "Before further modules". Install with
   `FREQ=400000 IMAGE=release tools/swd/install.sh` (one contact; **untested on hardware**, the
   stock path with the option reload has never run) or the single steps in docs/INSTALL-PROBE.md.
   Mains off, probe only on a dead board.
5. Optional: zero-cross delay measurement (`docs/ZC-DELAY-MEASUREMENT.md`; the DIAG build
   declines the ZTU's update offers, so it stays until `make flash-all` over SWD).
6. Parked: module button gestures. (Detached wall switch mode: in 2.2.0.) Next on the ZTU side:
   Zigbee Scenes and direct binding of the wall switch (designed 2026-11-19; nothing needed in
   the MCU beyond 2.2.0's fast press).
7. Done 2026-11-18: `ZTU_DISPATCH_TOKEN` is set; a re-run of v2.1.12's hand-over job started
   py32-bump in the fork, which saw 2.1.12 already carried and changed nothing. Releases now use
   `NO_TRIGGER=1 tools/release/release.sh`. Renew the token before it expires (a failing
   hand-over does not fail the release; `release.sh` without `NO_TRIGGER` still works).

## Safety rules

The board is not isolated from the mains. Never a probe, laptop or earthed scope on it while
it is on mains; mains off and wait a minute before SWD. The SIM build never goes on mains.
