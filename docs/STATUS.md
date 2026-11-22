# Open Dimmer: project status

The state of the project for contributors: what is released and tested, the datapoints, the
behaviour, how to build and release, and what is open. Users start from the
[README](../README.md) and the [install guide](INSTALL.md).

Last updated: 2026-11-20.

> [!CAUTION]
> **The board is not isolated from the mains.** Never connect a programmer, a computer or an earthed
> oscilloscope while it is on the mains. Disconnect the mains and wait a minute before SWD work.
> A `SIM=1` build never goes on the mains.

## Contents

- [At a glance](#at-a-glance)
- [Terms](#terms)
- [Datapoints](#datapoints)
- [Behaviour](#behaviour)
- [Development](#development)
- [Checklists](#checklists)
- [Open items](#open-items)

## At a glance

| | |
|---|---|
| Release | **2.2.7**, build 1200 |
| Size | app 13 728 of 14 336 bytes; bootloader 3 364 of 4 096 bytes |
| Bootloader checksum | CRC-32 `0x7D215316` (health DP 136) |
| Zigbee firmware | fork [`patapovich/tuya-zigbee-switch`](https://github.com/patapovich/tuya-zigbee-switch), branch `home-dimmer`; CI builds the OTA images on `home-dimmer_build` for every change |

The Zigbee firmware carries the newest release. It installs Open Dimmer over the two soldered
wires and updates a dimmer chip that runs an older Open Dimmer; it never writes over a stock
dimmer chip without the wires.

**Tested on hardware:**

- two-wire install on a stock unit, also with power cuts in every install phase;
- updates over Zigbee with both channels lit; a mains cut in the middle recovered;
- bootloader rewrite over the two wires; re-flash with an SWD programmer keeping the settings;
- bootloader probe, rescue, button and link watchdog;
- Zigbee Scenes and wall switch bindings.

Not yet tested: see [Open items](#open-items).

## Terms

| Term | Meaning |
|---|---|
| Dimmer chip, MCU | the PY32F002A on the main board; runs Open Dimmer |
| Zigbee module, ZTU | the Tuya ZTU (TLSR8258) radio module; runs the fork's Zigbee firmware |
| App | Open Dimmer itself, updated over Zigbee |
| Bootloader | installs every app update; written only over SWD and stays |
| DP | Tuya datapoint, the dimmer chip's interface to the Zigbee module |

## Datapoints

DPs 1..14 are the stock ones and keep their meaning; the rest are Open Dimmer's. "a / b" is
channel 1 / channel 2.

### Per channel

| DP | What | Values |
|---|---|---|
| 1 / 7 | on | on, off |
| 2 / 8 | level | – |
| 3 / 9 | min | physical conduction limit, for every curve as on the stock chip |
| 4 / 10 | switch type | 0 toggle, 1 state, 2 momentary |
| 5 / 11 | max | as min |
| 6 / 12 | countdown | auto-off in s, up to a day; not saved; 0 when an off ends it |
| 101 / 102 | dimming curve | incandescent, led, led_deep, led_deeper, led_soft (default), linear; custom (6, 2.2.8: the table of DPs 174.., led_soft until it came) |
| 103 / 104 | power-on | – |
| 105 / 106 | wall switch gesture (report) | 1 single, 2 double, 3 triple, 4 hold, 5 release, 6 press (fast press) |
| 150..154 / 155..159 | fade and hold | fade on ms, fade off ms, fade-off slow zone %, hold ms, hold slow zone % |
| 160 / 161 | transition | ms, for the network changes of the next second; not saved |
| 162 / 163 | wall switch mode | 0 normal, 1 detached (only reports), 2 off |
| 164 / 165 | level after a power cut | 0: the last level |
| 166 / 167 | identify | blink for that many s; 0 stops; not saved |
| 168 / 169 | start boost | ms, steps of 50, up to 750 |
| 170 / 171 | fast press | in detached mode a momentary press is reported at once |
| 172 / 173 | lamp compensation | value units at the min, steps of 4, up to 252; 0: none (2.2.8) |
| 174..176 / 177..179 | custom curve | raw, 6 little-endian u16 each: points 0-5, 6-11, 12-16 (a sixth: none) of 17 outputs 0..10000 at 0, 1/16 .. 1 of min..max; used when 0 first, 10000 last, never falling (else led_soft); saved with the settings (the record's extension), in the dump, a write answered with the part as kept (2.2.8) |
| 180 | wall switch options (both channels) | bits set = off (0, all on, the default): 0 / 1 a long press from off does not rise from the min (on at its level, the hold goes on from there), 2 / 3 a double tap does not take the light to full; written as mask << 8 \| bits (2.2.8, saved, in the first dump frame) |
| 181 | network reset presses (module wide) | value 0..255: that many closures of one wall switch (any type, normal or detached; never a switch set off), each within 10 s of the first, send the leave and pairing request (0x03), as the stock MCU with its fixed 8; the switch's channel blinks 3 s. 0: never; 1..7 act as 8 (the stock MCU's count). Not saved: 8 at every start, in the first dump frame; the fork's ZTU writes its multi-press reset count (2.2.8) |

During a hold the level is reported every 0.5 s.

### Module wide

| DP | What | Values |
|---|---|---|
| 14 | power-on, both channels | written: both; reported: channel 1's |
| 130..135 | health, 3 s after the module asked who we are (its start or ours), then every 2 min | uptime min, reset cause (RCC_CSR >> 24), lock losses, ignored edges, settings saves, UART receive losses |
| 136 | bootloader checksum | CRC-32 of the boot sector |
| 137 | boot word at start | tries, probe answer time, rescue / button / link flags; bits 24 / 25 settings ring layout ([BOOTLOADER.md](BOOTLOADER.md) section 9) |
| 138 | die temperature | °C, about ±10 °C absolute |
| 139 | its calibration | – |
| 110..123 | diagnostics | DIAG / MEASURE builds only |
| 0xA0..0xA3 | Open Dimmer commands | update notify / block / result, identity |

## Behaviour

**Fades**

- On from dark: starts at min, 950 ms to full.
- Off: 950 ms, slower in the lowest 12 % above min, 80 ms at min, then dark.

**Wall switch**

- Momentary: a tap from dark goes quickly to the stored level. A hold from dark rises slowly from
  min. A hold while lit that starts within 2 s of the last one's end goes the other way (a
  correction); any other takes the way learnt for the quarter of min..max it starts in, at first
  towards the far end. A learnt hold let go within 1.2 s and turned round at once was the wrong
  way: that quarter learns the other one (a quarter that was just confirmed needs a second miss).
  A hold taken to the end counts as right. The learnt ways survive a power cut. Slow near min.
  Release stops at once and saves the level. A hold ends by itself after 2 × hold time + 2 s.
- Toggle: every change toggles.
- State: on while closed; set at once when the type changes to it.
- An input closed at power-up is no press.
- The S1 input stays low ~0.1 s after release: a very fast double tap merges in hardware.

**Module button:** a press toggles both channels (any on → all off); held ≥ 3 s it rejoins.

**Restarts:** after a watchdog, fault or update restart the light comes back as it was (an update
keeps it dark for the few seconds the bootloader writes). A second
such reset within a minute, a power cut or a brown-out takes the power-on setting.

**Mains frequency:** the zero-cross tracker takes 40..70 Hz, and the times scale with the half
period.

## Development

### Where things are

| Path | What |
|---|---|
| `src/`, `boot/` | the app and the bootloader |
| `tests/` | host tests (`make test`, with the bootloader harness) |
| `tools/` | image and release tools; `swd/` SWD programmer install; `ha/` Zigbee2MQTT / Home Assistant helpers |
| `docs/` | [INSTALL.md](INSTALL.md), [INSTALL-PROGRAMMER.md](INSTALL-PROGRAMMER.md), [BOOTLOADER.md](BOOTLOADER.md) (design record), [stock-firmware-analysis.md](stock-firmware-analysis.md), [ZC-DELAY-MEASUREMENT.md](ZC-DELAY-MEASUREMENT.md) |

### Flash layout

| Region | Address | Size |
|---|---|---|
| Bootloader | `0x08000000` | 4 KB |
| App | `0x08001000` | up to 14 KB |
| Settings ring | `0x08004800` | 2 KB |

Update rules: [BOOTLOADER.md](BOOTLOADER.md).

### CI

`.github/workflows/ci.yml` runs the host and bootloader tests, the normal, DIAG, MEASURE and SIM
builds, the layout and header checks and a size report.

### Release

1. Bump `FW_VERSION` and `FW_BUILD` (+100 per release) in `src/tuya.h`.
2. Add the version's section to `CHANGELOG.md`.
3. Run `tools/release/release.sh`: it checks, tags, waits for the CI release and carries the
   release into the fork. The fork then builds the OTA images by itself.

Deploy over the air from Zigbee2MQTT, or with `tools/ha/deploy_ota.sh`
(see [tools/ha/README.md](../tools/ha/README.md)).

## Checklists

### Converter change

Before deploying a changed Zigbee2MQTT converter:

- [ ] Dump the exposes (`tools/ha/expose_dump.js`) for an Open Dimmer, a stock and an unknown
      dimmer chip, and diff them against the deployed converter.
- [ ] Check that every other model's definition is unchanged.
- [ ] Snapshot Home Assistant's entity registry before and after the Zigbee2MQTT restart: no
      entity removed, added or renamed unless intended.

### Rollout to further modules

- [ ] The Zigbee firmware first, with the stock dimmer chip still in, one module at a time.
- [ ] Then Open Dimmer (two wires or SWD programmer).
- [ ] A new Open Dimmer version goes to further modules only after a test module has run it
      and taken an update over Zigbee from it.

## Open items

| # | Item | State |
|---|---|---|
| 1 | 60 Hz mains and the `_TZE204_jtbgusdc` variant | untested on hardware |
| 2 | SWD programmer path: `install.sh`'s option reload on a stock module | never run (the steps by hand have) |
| 3 | Watchdog and fault light restore | host tests only |
| 4 | Long trial: read the health DPs 131, 132, 133 and 135 now and then | ongoing |
| 5 | Fade and hold defaults | settle after more use |
| 6 | Zero-cross delay measurement ([ZC-DELAY-MEASUREMENT.md](ZC-DELAY-MEASUREMENT.md)) | optional |
| 8 | Predictive first hold direction: learnt per quarter of the range from quick reversals | done (2.2.5), tested on hardware |
| 9 | Zigbee firmware: hold reports after the module's wall-switch OnLevel write were held back about 2 s | fixed in the Zigbee firmware (a hold gesture ends the holdoff), tested on hardware |
| 10 | Zigbee firmware: right after a restart it reported the light off before it knew the dimmer chip's state | fixed in the Zigbee firmware (reports wait until every dimmer knows its state), tested on hardware |
| 11 | Download counts: hubs fetched the OTA images as raw files, which GitHub does not count | done: each build's OTA images are release assets of the fork and the OTA index points to them (download counts per asset) |
| 12 | Zigbee firmware: an Open Dimmer update made the light go off and on (the bootloader keeps it dark while it writes) | fixed in the Zigbee firmware: updates wait for the light to be off (a minute after the off, a day at most), tested on hardware |
| 13 | A wall switch, the module button or the auto-off used within a second of a transition time from the network took that time (the light went off or came on over e.g. 45 s) | fixed in 2.2.7 |
| 14 | A hold from dark let go during a start boost longer than 0.4 s left the light at full | fixed in 2.2.7 |
| 15 | Zigbee firmware: the update wait did not run for a light that was off when the module started, a transition to off could outlast it, and an offer could go to a chip in its bootloader | fixed in the Zigbee firmware (70 s after the off or the module's start, offers only to a running app), tested on hardware |
| 16 | Zigbee firmware: a level command during a wall switch hold held the hold's level back for 3 s; Stop did not stop a transition and Move / Step started from its end; an auto-off countdown was lost when the dimmer chip restarted | fixed in the Zigbee firmware; the hold and Stop / Move / Step tested on hardware, the countdown in the simulator only |
| 17 | Zigbee firmware, two-wire install: `abort` or a second `install` after the read protection was removed dropped the install; a second power cut after one during the option bytes could end it; a taken-up install wrote the bootloader without identifying the chip | fixed in the Zigbee firmware (simulator tests: the usual install paths send the same SWD traffic as before); bench tests of the changed paths on a spare module open |
