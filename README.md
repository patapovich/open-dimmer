# Open Dimmer

Open-source firmware for the PY32F002A dimming MCU of the **AVATTO ZDMS16-2** two-channel
Zigbee dimmer module, with a Zigbee firmware for its radio: the ZDMS16-2 build of
[patapovich/tuya-zigbee-switch](https://github.com/patapovich/tuya-zigbee-switch) (branch
`home-dimmer`, a fork of [romasku/tuya-zigbee-switch](https://github.com/romasku/tuya-zigbee-switch)).
For Zigbee2MQTT users. No debug probe needed.

## Install

1. **Zigbee firmware over the air** from Zigbee2MQTT, no opening
   ([INSTALL step 1](docs/INSTALL.md#1-zigbee-firmware-over-the-air)).
2. **Solder two wires**, module off the mains: ZTU pad D3 → CN2 pad SWC, ZTU pad D7 → CN2 pad
   SWD ([INSTALL step 2](docs/INSTALL.md#2-solder-two-wires), with photos).
3. **Close the module and switch it on.** Open Dimmer installs itself; within about 10 s
   Zigbee2MQTT shows `mcu_firmware` "Open Dimmer". Press Reconfigure on the device.

After that every update goes over Zigbee. The MCU's settings go back to Open Dimmer's defaults.

> **Mains voltage.** The low-voltage side is not isolated from the mains. Open the module only
> with all mains wires disconnected, close it before reconnecting them, and never connect a
> probe, a computer or an earthed oscilloscope to a module on the mains.

## Why Open Dimmer

The stock firmware dims linearly at one fixed rate, has one power-on setting for both channels
and reports only on, off and level. Open Dimmer adds, per channel:

- dimming curves (led_soft, led, led_deep, led_deeper, incandescent, linear);
- fade on / fade off / hold times with slow zones near the minimum; Zigbee transitions;
- wall switch gestures as Zigbee2MQTT `action` (single, double, triple, hold, release, fast
  press) and wall switch modes normal / detached / off;
- power-on behaviour, the level after a power cut, start boost for LED drivers, auto-off
  countdown, identify;
- health reports (uptime, reset cause, zero-cross and UART losses, die temperature);
- updates over Zigbee through a 4 KB bootloader: checked before anything is erased, the light
  kept, a rescue window for a crashing app.

It needs the fork's Zigbee firmware.

### The wall switch

| | Stock firmware | Open Dimmer |
|---|---|---|
| Tap | on / off | on / off; from dark quickly to the stored level |
| Hold | dims at one fixed rate, up and down by turns | from dark: rises slowly from the minimum (very dim levels are easy to pick); while lit: up or down by turns, the first towards the far end; slow near the minimum; the hold time is a setting |
| Double, triple, hold, release | not reported | Zigbee2MQTT `action`; can drive bound lights |
| Modes | the switch always drives the light | normal, detached (only reports: automations, bound lights), off |
| Stuck or shorted input | – | a hold ends by itself |

## Zigbee firmware features

With the stock MCU and with Open Dimmer:

- two dimmable lights (`l1`, `l2`): min / max (level scaled to the range), switch type,
  power-on behaviour, OnLevel, remote dimming at the remote's rate;
- Zigbee Scenes (16, kept across power cuts) and groups;
- direct control without the coordinator: lights and groups bound to a wall switch's own
  endpoint (3 / 4) follow its on, off and hold;
- OTA updates of itself, conversion from the stock Zigbee firmware, MCU link diagnostics, a
  Zigbee2MQTT converter.

With Open Dimmer:

- carries it and updates the MCU over the internal UART; installs it over the two wires;
- its settings in Zigbee2MQTT, plus the wall switch's own OnLevel, ExecuteIfOff and the level
  after a power cut;
- gestures drive the bound lights (detached mode); per channel `bind_mode` and double / triple
  press actions (full, off, Recall Scene 1 / 2).

Details: [docs/STATUS.md](docs/STATUS.md) (datapoints, flash layout, open items), the fork's
[build changelog](https://github.com/patapovich/tuya-zigbee-switch/blob/home-dimmer/docs/changelog_home_dimmer.md).

## Tested hardware

ZDMS16-2 `_TZE284_jtbgusdc` (Tuya ZTU / TLSR8258, PY32F002AF15P with stock firmware 1.0.7),
230 V / 50 Hz, LED lamps and push buttons on both channels. Not tested: 60 Hz, the
`_TZE204_jtbgusdc` variant, other DMS16 versions. Reports welcome.

## Build

Needs `arm-none-eabi-gcc` and the PY32F0 SDK from
[IOsetting/py32f0-template](https://github.com/IOsetting/py32f0-template) (CI pins the commit).

```
make test                                  # host tests, the bootloader harness included
make SDK=<py32f0-template>/Libraries       # build/zdms16_py32.img (+ bootloader, full image)
make DIAG=1 | MEASURE=1 | SIM=1            # diagnostics / measurement / bench builds
make flash-all                             # SWD (pyOCD), board off the mains
```

A `vX.Y.Z` tag publishes a release that the Zigbee firmware carries (`tools/release/release.sh`).
`src/` firmware, `boot/` bootloader, `tests/`, `tools/` (image, SWD, Home Assistant helpers), `docs/`.

## License

MIT, see [LICENSE](LICENSE), except `docs/img/dms16-pinout-elektroda.png` (picture by divadiow,
[elektroda.com topic4066696](https://www.elektroda.com/rtvforum/topic4066696.html), copied with
credit). Not affiliated with AVATTO, Tuya or Puya.
