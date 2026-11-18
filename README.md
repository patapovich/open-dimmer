# Open Dimmer

Open Dimmer is open-source firmware for the PY32F002A dimming MCU of the AVATTO ZDMS16-2
two-channel Zigbee dimmer module. The Zigbee side runs a fork of
[romasku/tuya-zigbee-switch](https://github.com/romasku/tuya-zigbee-switch) (branch
`home-dimmer` of `patapovich/tuya-zigbee-switch`), which carries Open Dimmer and updates the
MCU over the module's internal UART, so after the first SWD flash every update goes over Zigbee.
The stock MCU firmware is never touched by the update.

In Zigbee2MQTT a module with Open Dimmer shows `mcu_firmware` "Open Dimmer" and the plain
version in `mcu_version` (for example 2.1.11); with the stock MCU `mcu_firmware` shows
"Tuya (stock)".

> **Mains voltage.** The module's low-voltage side is not isolated from the mains. Never connect
> a debug probe, a computer or an earthed oscilloscope while it is connected to the mains: unplug,
> wait, then connect. The bench (`SIM=1`) build must never run on the mains.

## Features

- Phase dimming locked to the zero cross, min/max as physical conduction limits for every curve
  (the same meaning as in the stock firmware).
- Dimming curves: led_soft (default), led (CIE lightness), led_deep, led_deeper, incandescent, linear (stock).
- Per-channel power-on behaviour and per-channel saved fade / hold settings.
- Wall switch: toggle, state or momentary; with momentary a tap from dark goes on at once, a hold
  from dark rises slowly from the minimum, the hold stops the moment the switch is released.
- Gestures single / double / triple / hold / release reported to Zigbee2MQTT as `action`.
- Update over Zigbee: 4 KB bootloader, nothing erased before the first block, the light state is
  kept over the update, only newer images are taken. A watchdog or fault restart keeps the
  light state too (not yet tested on hardware); a power cut follows the power-on setting.
- Health report (uptime, reset cause, zero-cross lock losses, UART receive losses) and
  measurement builds that report statistics over Zigbee with nothing wired to the board. The
  measurement and diagnostics builds decline every update the Zigbee firmware offers, so they
  stay until the next SWD flash (`make flash-all` brings back a normal build).

**Installation:** [docs/INSTALL.md](docs/INSTALL.md): first the Zigbee firmware over the air
([section 0](docs/INSTALL.md#0-zigbee-side-first)), then once per module over SWD, in one contact with
`tools/swd/install.sh` (read protection off, brown-out reset, flash, check); then updates over Zigbee.

See [docs/STATUS.md](docs/STATUS.md) for the datapoints, the flash layout, the build and deploy
recipe and the open items.

## Tested hardware

So far tested on one module:

| | |
|---|---|
| Device | **AVATTO ZDMS16-2**, 2-gang Zigbee dimmer module |
| Zigbee model | stock: `TS0601`, manufacturer `_TZE284_jtbgusdc`; with the fork's Zigbee firmware: **`TS0601-ZDMS16-2`**, manufacturer `jtbgusdc` (as Zigbee2MQTT shows it) |
| Radio | Tuya ZTU module (Telink TLSR8258), running the fork's Zigbee firmware 1.1.3 |
| Dimming MCU | Puya **PY32F002AF15P** (TSSOP20), stock firmware 1.0.7 (read-protected, replaced by Open Dimmer) |
| Mains | 230 V / 50 Hz |
| Load and switch | channel 1 (L1): a dimmable LED lamp, a momentary push button between L and S1; channel 2 (L2 / S2): a lamp and a push button too (on, off, hold dimming and gestures checked) |

Not tested yet: 60 Hz mains (unit tests only), the
`_TZE204_jtbgusdc` variant (listed as the same board, not verified), other DMS16 versions (the WiFi
DMS16-W1, board MK-WC1H16A-A, was only read for the analysis). Reports of other modules are welcome.

## Build

Needs `arm-none-eabi-gcc` and the PY32F0 SDK from
[IOsetting/py32f0-template](https://github.com/IOsetting/py32f0-template) (CI pins the commit).

```
make test                                  # host unit tests, the bootloader harness included
make test-boot                             # the bootloader harness alone (tests/boot)
make SDK=<py32f0-template>/Libraries       # build/zdms16_py32.img (+ bootloader, full image)
make check-boot                            # the bootloader's layout (boot_id, write range, size)
make DIAG=1 | MEASURE=1 | SIM=1            # diagnostics / measurement / bench builds
make flash-all                             # SWD (pyOCD), board off the mains
```

`tools/embed_ztu.py` turns an image into the C file the Zigbee firmware carries.

CI builds and tests every push; a `vX.Y.Z` tag publishes a release that the Zigbee firmware fork
picks up and builds into its OTA image (`tools/release/release.sh`, see docs/STATUS.md).

## Layout

| Path | |
|---|---|
| `src/` | firmware (`dim_core`, `input_core`, `gesture_core` are hardware-free and unit tested) |
| `boot/` | bootloader |
| `tests/` | host tests; `tests/boot/` runs the real bootloader against models of the chip and the ZTU |
| `tools/` | curve generator, image tools, SWD helpers (`tools/swd`), HA / Z2M helpers (`tools/ha`) |
| `docs/` | status, review, measurement notes, analysis of the stock firmware |

## License

MIT, see [LICENSE](LICENSE), except `docs/img/dms16-pinout-elektroda.png` (picture by divadiow,
[elektroda.com topic4066696](https://www.elektroda.com/rtvforum/topic4066696.html), copied with
credit). Not affiliated with AVATTO, Tuya or Puya.
