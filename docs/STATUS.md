# Open Dimmer: status

Open-source firmware for the PY32F002A dimming MCU of the AVATTO ZDMS16-2, installed and updated
by the Zigbee firmware of the fork `patapovich/tuya-zigbee-switch` (branch `home-dimmer`).
State of 2026-11-20.

## Current state

| | |
|---|---|
| Release | **2.2.4** (build 900): app 13 500 of 14 336 bytes; bootloader 3 364 of 4 096 bytes, boot sector CRC-32 `0x7D215316` (health DP 136) |
| Zigbee firmware | fork `home-dimmer`; CI builds the OTA images on `home-dimmer_build` for every change. It carries the newest release and updates an Open Dimmer MCU that runs an older one (never a stock MCU); it installs Open Dimmer over two soldered wires |
| Tested on hardware | two-wire install on a stock unit, also with power cuts in every install phase; updates over Zigbee (with both channels lit; a mains cut mid-update recovered); SWD bootloader rewrite over the two wires; probe re-flash keeping the settings; bootloader probe, rescue, button and link watchdog; Zigbee Scenes and wall switch bindings |
| Not yet tested | see [Open items](#open-items) |

## Where things are

| Path | What |
|---|---|
| `src/`, `boot/` | the app and the 4 KB bootloader |
| `tests/` | host tests (`make test`, with the bootloader harness) |
| `tools/` | image and release tools, `swd/` (probe install), `ha/` (Zigbee2MQTT / Home Assistant helpers) |
| `docs/` | [INSTALL.md](INSTALL.md), [INSTALL-PROBE.md](INSTALL-PROBE.md), [BOOTLOADER.md](BOOTLOADER.md) (design), `stock-firmware-analysis.md`, `ZC-DELAY-MEASUREMENT.md` |

## Datapoints

The stock DPs 1..14 keep their meaning; the rest are Open Dimmer's. "a / b": channel 1 / 2.

| DP | Meaning |
|---|---|
| 1..6 / 7..12 | on, level, min, switch type, max, countdown (an auto-off in s, up to a day, not saved; 0 when an off ends it). A hold reports the level every 0.5 s |
| 14 | power-on, both channels (written: both; reported: channel 1's) |
| 101 / 102 | dimming curve: incandescent, led, led_deep, led_deeper, led_soft (default), linear |
| 103 / 104 | power-on per channel |
| 105 / 106 | wall switch gesture: 1 single, 2 double, 3 triple, 4 hold, 5 release, 6 press (fast press) |
| 130..135 | health every 2 min: uptime min, reset cause (RCC_CSR >> 24), lock losses, ignored edges, settings saves, UART receive losses |
| 136 / 137 | CRC-32 of the boot sector; the boot word at start (tries, probe answer time, rescue / button / link flags; bits 24 / 25 settings ring layout; BOOTLOADER.md section 9) |
| 138 / 139 | die temperature °C (about ±10 °C absolute) / its calibration |
| 150..154 / 155..159 | fade on ms, fade off ms, fade-off slow zone %, hold ms, hold slow zone % |
| 160 / 161 | transition time in ms for the network changes of the next second (not saved) |
| 162 / 163 | wall switch mode: 0 normal, 1 detached (only reports), 2 off |
| 164 / 165 | level after a power cut (0: the last level) |
| 166 / 167 | identify: blink for that many s (0 stops; not saved) |
| 168 / 169 | start boost ms (steps of 50, up to 750) |
| 170 / 171 | fast press: in detached mode a momentary press is reported at once |
| 110..123 | DIAG / MEASURE builds only |
| 0xA0..0xA3 | Open Dimmer's own commands: update notify / block / result, identity |

Min and max are physical conduction limits for every curve, as on the stock MCU.

## Behaviour

- On from dark: starts at min, 950 ms to full. Off: 950 ms, slower in the lowest 12 % above min,
  80 ms at min, then dark.
- Momentary switch: tap from dark quickly to the stored level; hold from dark rises slowly from
  min; hold while lit goes up or down by turns (the first towards the far end), slow near min;
  release stops at once and the level is saved. A hold ends by itself after 2 × hold time + 2 s.
- Toggle switch: every change toggles. State switch: on while closed, set at once when the type
  changes to it. An input closed at power-up is no press.
- The S1 input stays low ~0.1 s after release: a very fast double tap merges in hardware.
- Module button: both channels (any on → all off); held ≥ 3 s: rejoin.
- After a watchdog, fault or update restart the light comes back as it was; a second such reset
  within a minute, a power cut or a brown-out takes the power-on setting.
- 50 / 60 Hz: the zero-cross tracker takes 40..70 Hz and the times scale with the half period.

## Flash, build, release

- Flash: bootloader 4 KB @0x08000000, app up to 14 KB @0x08001000, settings ring 2 KB
  @0x08004800. Layout and update rules: [BOOTLOADER.md](BOOTLOADER.md).
- CI (`.github/workflows/ci.yml`): host and bootloader tests, the normal, DIAG, MEASURE and SIM
  builds, layout and header checks, size report.
- Release: bump `FW_VERSION` and `FW_BUILD` (+100 per release) in `src/tuya.h`, add the
  `CHANGELOG.md` section, run `tools/release/release.sh` (checks, tag, CI release, carries it
  into the fork). The fork then builds the OTA images by itself.
- Deploy: over the air from Zigbee2MQTT, or `tools/ha/deploy_ota.sh` (see `tools/ha/README.md`).

## Converter changes: checklist

Before deploying a changed Zigbee2MQTT converter:
1. Dump the exposes (`tools/ha/expose_dump.js`) for an Open Dimmer, a stock and an unknown MCU
   and diff them against the deployed converter.
2. Check that every other model's definition is unchanged.
3. Snapshot Home Assistant's entity registry before and after the Zigbee2MQTT restart: no
   entity removed, added or renamed unless intended.

## Rollout to further modules

The Zigbee firmware first, with the stock MCU still in, one module at a time; then the MCU (two
wires or probe). A PY32 version goes to further modules only after a test module has run it and
taken an update over Zigbee from it.

## Open items

1. 60 Hz mains and the `_TZE204_jtbgusdc` variant: untested on hardware.
2. Probe path: `install.sh`'s option reload has never run on a stock module (the steps by hand
   have).
3. Watchdog and fault light restore: host tests only.
4. Long trial: read the health DPs now and then (131, 132, 133, 135).
5. Fade and hold defaults: settle them after more use.
6. Optional: zero-cross delay measurement (`ZC-DELAY-MEASUREMENT.md`).
7. Parked: module button gestures.

## Safety rules

The board is not isolated from the mains. Never a probe, a computer or an earthed scope on it
while it is on the mains; mains off and wait a minute before SWD. The SIM build never goes on
the mains.
