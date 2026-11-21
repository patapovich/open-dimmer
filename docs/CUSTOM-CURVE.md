# A custom dimming curve for your lamp

Open Dimmer's built-in curves (`led_soft`, `led`, `led_deep`, `led_deeper`, `incandescent`,
`linear`) are made for lamps in general. Every LED lamp's driver spreads its light in its own
way, so with a built-in curve a lamp may change a lot on some part of the range and hardly at
all on another. From Open Dimmer **2.2.8** a channel can have a **custom curve**: a table of 17
points measured for its lamp, so that every step of the level is the same step of perceived
brightness, from the minimum to the maximum.

`tools/custom_curve.py` does the measurement and writes the curve.

## What you need

- Open Dimmer 2.2.8 or later on the dimmer, the Zigbee firmware and converter that come with it
  (Zigbee2MQTT then shows `custom_curve` and the `custom` choice of `dimming_curve`).
- Something that measures light:
  - an **illuminance sensor** in Zigbee2MQTT (a motion or presence sensor with `illuminance`),
    put right next to the lamp, facing it; or
  - a **light meter** (a phone app works) with `--manual`: the tool asks you for each reading.
- Python 3. With Home Assistant nothing else (`--ha`, a long-lived access token in `HA_TOKEN`);
  with MQTT directly `pip install paho-mqtt` (`--mqtt`, the password in `MQTT_PASSWORD`).
- A dark room: daylight and other lamps change the readings. Nobody walking in front of the
  sensor.
- Set the channel's `min_level` and `max_level` first. The curve is made for that range: measure
  again after changing them.
- Switch off automations that change the lamp while it is measured (Adaptive Lighting, motion
  lights).

## Run it

```sh
HA_TOKEN=... python3 tools/custom_curve.py --ha http://homeassistant.local:8123 \
    --device bedroom-dimmer --channel l1 --sensor bedroom-presence --save bedroom.json --apply
```

It measures 14 levels with the linear curve, each switched on from dark (about 7 minutes), puts
the lamp's curve back, computes the custom curve, writes it and selects it. Without `--apply` it
only prints the curve; `--load bedroom.json` computes it again from a saved measurement.

With a light meter instead of a sensor:

```sh
HA_TOKEN=... python3 tools/custom_curve.py --ha http://homeassistant.local:8123 \
    --device bedroom-dimmer --channel l1 --manual --hold 6 --apply
```

## A sensor that is not linear

Many cheap sensors do not report real lux: the reading grows more slowly than the light (it
looks roughly logarithmic). The curve then comes out wrong. Calibrate such a sensor once with a
light meter at the same place: note the sensor's reading and the meter's lux in the dark, at the
minimum, at a few levels and at full, and give them as a JSON list:

```json
[[257, 0.64], [379, 0.80], [633, 1.22], [1768, 5.5], [2601, 24.5]]
```

```sh
... --sensor bedroom-presence --calibration calibration.json --apply
```

## What the curve is

17 outputs from 0 to 10000 at 0, 1/16, 2/16 .. 1 of the channel's min..max range: 0 is the
minimum's conduction, 10000 the maximum's. You can also type one in Zigbee2MQTT (`custom_curve`,
comma-separated); it is refused unless it starts at 0, ends at 10000 and never falls. The
dimmer chip saves it with its settings (an update keeps it; after a downgrade to a release before
2.2.8 the curve is linear). Selected without a valid table, `custom` dims like `led_soft`. An
empty `custom_curve` removes it.

`python3 tools/custom_curve.py --self-test` checks the maths on a made-up lamp.
