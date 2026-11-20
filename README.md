# Open Dimmer

Open-source firmware for the **AVATTO ZDMS16-2** two-channel Zigbee dimmer: Open Dimmer for its
dimming MCU, plus a Zigbee firmware for its radio (the ZDMS16-2 build of
[patapovich/tuya-zigbee-switch](https://github.com/patapovich/tuya-zigbee-switch), a fork of
[romasku/tuya-zigbee-switch](https://github.com/romasku/tuya-zigbee-switch)). For Zigbee2MQTT.

## Install

1. **Zigbee firmware over the air** from Zigbee2MQTT, no opening
   ([INSTALL step 1](docs/INSTALL.md#1-zigbee-firmware-over-the-air)).
2. **Solder two wires**, module off the mains
   ([INSTALL step 2](docs/INSTALL.md#2-solder-two-wires), with photos).
3. **Close the module and switch it on.** Open Dimmer installs itself in a few seconds.

Updates then go over Zigbee. The dimmer's settings go back to defaults.

> **Mains voltage.** The low-voltage side is not isolated from the mains. Open the module only
> with all mains wires disconnected and close it before reconnecting them.

## Why Open Dimmer

The stock firmware dims linearly at one fixed rate, shares one power-on setting between the
channels and reports only on, off and level. Open Dimmer adds, per channel:

- dimming curves for LED and incandescent lamps;
- adjustable fade and hold times; Zigbee transitions;
- wall switch gestures (single, double, triple, hold) as Zigbee2MQTT actions; modes normal,
  detached and off;
- power-on behaviour and level, start boost for LED drivers, auto-off timer, identify;
- health reports;
- safe updates over Zigbee: the light stays as it was, a failed update reinstalls itself.

### The wall switch

The stock firmware is quirky in use: it reads the wall switch inconsistently and reacts slowly.

| | Stock firmware | Open Dimmer |
|---|---|---|
| Reading the switch | inconsistent | debounced; contact bounce and a switch held at power-up are ignored |
| Brightness changes | one fixed fade (2 s); Zigbee transitions ignored | adjustable fades; Zigbee transitions |
| Tap | on / off | on / off; from dark quickly to the stored level |
| Hold | slow fixed rate (~6 s over the range) | ~3 s over the range (adjustable); from dark it starts at the minimum; slow near the minimum |
| Double, triple, hold | not reported | Zigbee2MQTT actions; can drive bound lights |
| Modes | the switch always drives the light | normal, detached (only reports), off |
| Stuck input | – | a hold ends by itself |

## Zigbee firmware features

With the stock MCU and with Open Dimmer:

- two dimmable lights with min / max, switch type, power-on behaviour and on level;
- Zigbee Scenes and groups;
- direct control: lights and groups bound to the wall switch follow its on, off and hold, also
  without the coordinator;
- updates over the air, conversion from the stock Zigbee firmware.

With Open Dimmer:

- installs and updates Open Dimmer by itself;
- every Open Dimmer setting in Zigbee2MQTT;
- wall switch gestures drive bound lights; double and triple press actions, scene recall
  included.

More: [docs/STATUS.md](docs/STATUS.md), the fork's
[changelog](https://github.com/patapovich/tuya-zigbee-switch/blob/home-dimmer/docs/changelog_home_dimmer.md).

## Tested hardware

AVATTO ZDMS16-2 (`_TZE284_jtbgusdc`), 230 V / 50 Hz, LED lamps, push buttons. Not tested: 60 Hz,
the `_TZE204_jtbgusdc` variant. Reports welcome.

## Build

Needs `arm-none-eabi-gcc` and the PY32F0 SDK from
[IOsetting/py32f0-template](https://github.com/IOsetting/py32f0-template) (CI pins the commit).

```
make test                                  # host tests
make SDK=<py32f0-template>/Libraries       # build/zdms16_py32.img
```

A `vX.Y.Z` tag publishes a release that the Zigbee firmware carries (`tools/release/release.sh`).

## License

MIT, see [LICENSE](LICENSE), except `docs/img/*-elektroda.png` (pictures by divadiow,
[elektroda.com topic4066696](https://www.elektroda.com/rtvforum/topic4066696.html), copied with
credit). Not affiliated with AVATTO, Tuya or Puya.
