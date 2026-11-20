# Installing Open Dimmer on an AVATTO ZDMS16-2

Three steps: the Zigbee firmware over the air, two soldered wires, power on. The module then
installs Open Dimmer by itself. With a debug probe instead: [INSTALL-PROBE.md](INSTALL-PROBE.md).

> **Mains voltage.** The low-voltage side is not isolated from the mains. Disconnect all mains
> wires before you open the module, and close it before you reconnect them.

> **Settings go back to defaults.** The install replaces the stock MCU firmware for good and
> with it the MCU's settings: min 20 %, max 100 %, switch type toggle, power-on off. Set them
> again in Zigbee2MQTT afterwards (a push button needs switch type momentary).

## 1. Zigbee firmware over the air

Convert the module from the stock Zigbee firmware with Zigbee2MQTT as the upstream guide
describes ([conversion](https://github.com/romasku/tuya-zigbee-switch/blob/main/docs/updating.md#conversion),
[external converters](https://github.com/romasku/tuya-zigbee-switch/blob/main/docs/updating.md#quirks--converters--extensions)),
but with the fork's files:

- OTA index (`ota.zigbee_ota_override_index_location`):
  `https://raw.githubusercontent.com/patapovich/tuya-zigbee-switch/home-dimmer_build/zigbee2mqtt/ota/index_router.json`
  (it lists only the ZDMS16-2; other tuya-zigbee-switch devices: [merge the indexes](#other-tuya-zigbee-switch-devices));
- converter `switch_custom.js` from
  [zigbee2mqtt/converters](https://github.com/patapovich/tuya-zigbee-switch/tree/home-dimmer_build/zigbee2mqtt/converters).

After the update the module can take up to ~6 minutes to come back (its LED stays dark; leave
the power on). When Zigbee2MQTT shows the model `TS0601-ZDMS16-2` and the build `1.1.3-…`,
**Interview** the device, then **Reconfigure** it. It now works with the stock MCU:
`mcu_firmware` shows "Tuya (stock)".

## 2. Solder two wires

Mains off, module open. Two thin insulated wires a few cm long (e.g. 30 AWG wire-wrap):

| ZTU pad | CN2 pad | Signal |
|---|---|---|
| D3 | SWC (pad 2) | SWCLK, PY32 PA14 |
| D7 | SWD (pad 1) | SWDIO, PY32 PA13 |

D3 and D7 are the first two pads at the antenna end of the ZTU's edge with C0, SWS, B6, A0 and
A1; nothing else is connected to them. CN2 is the row of four pads next to the PY32. No solder
bridges, and the wires touch nothing else.

![ZTU front view with D3 and D7 marked](img/ztu-d3-d7.png)

![CN2 next to the PY32: SWD, SWC, GND, VCC](img/cn2-pads-elektroda.png)

![The two wires from the ZTU down to CN2](img/two-wires-side.jpg)

More pictures: [the wires from the front](img/two-wires-angle.jpg),
[the board from below](img/board-bottom.jpg),
[PY32 pinout and CN2](img/dms16-pinout-elektroda.png). The CN2 picture above is cut from it
(both by divadiow, [elektroda.com topic4066696](https://www.elektroda.com/rtvforum/topic4066696.html);
not covered by this repository's license). The ZTU drawing is from the fork's
[ZTU front view](https://github.com/patapovich/tuya-zigbee-switch/blob/home-dimmer/docs/diagrams/telink/ZTU_front.png).

## 3. Power on

Close the module, reconnect it and switch the mains on. The lamps stay dark for a few seconds
while Open Dimmer installs; then Zigbee2MQTT shows `mcu_firmware` "Open Dimmer" and
`mcu_version` 2.x.y. Press **Reconfigure**: the Open Dimmer settings appear (per channel the
dimming curve, power-on behaviour, fade and hold times, wall switch gestures as `action`). The
shared `power_on_behavior` becomes one per channel: update automations that use it.

## If it does not work

- `mcu_firmware` says "power-cycle the module to install Open Dimmer": the wires were there
  before step 1. Switch the module off for a few seconds and on again.
- Still "Tuya (stock)": set the device option `mcu_flasher` to `show` and read
  `mcu_flasher_status`. `no answer (wires? power?)`: check the two joints, mains off. To start
  again: write `install` to `mcu_flasher_confirm`, within 60 s set `mcu_flasher` to `install`,
  and power-cycle the module when the status asks for it.
- A power cut during the install: switch on again, it goes on or starts over by itself.
- A `device_config` written by hand gets `SWD3D7;` once with the update; if you removed it
  later, add it back.

The automatic install runs once per module, at the first power-up that sees the wires. A module
that already runs Open Dimmer is left alone.

## Updates

Update the Zigbee firmware over the air as usual. It carries Open Dimmer and updates the MCU
by itself a few seconds after it restarts; the light comes back as it was. An app that fails
to start three times in a row makes the bootloader install the carried image again by itself.

**Button rescue** (going back to the image the Zigbee firmware carries, the case stays closed):
power off, hold the module button through the case, power on, let go within 5 s after the LED
lights. The LED blinks slowly while it waits and fast while it writes; with no other image the
light starts after ~30 s.

## Other tuya-zigbee-switch devices

Zigbee2MQTT takes only one override index, and the fork's lists only the ZDMS16-2. If other
devices run tuya-zigbee-switch firmware, merge both indexes into a local file: run this in the
Zigbee2MQTT data directory (next to `configuration.yaml`), set the option to `ota_index.json`,
and run it again for new releases (needs `curl` and `jq` 1.6+). The file is replaced only when
both downloads succeed.

```
curl -fsSL -o ota_upstream.json https://raw.githubusercontent.com/romasku/tuya-zigbee-switch/refs/heads/main/zigbee2mqtt/ota/index_router.json &&
curl -fsSL -o ota_fork.json https://raw.githubusercontent.com/patapovich/tuya-zigbee-switch/refs/heads/home-dimmer_build/zigbee2mqtt/ota/index_router.json &&
jq -s '(.[1] | map(.manufacturerName[]?)) as $m
       | [.[0][] | select(any(.manufacturerName[]?; IN($m[])) | not)] + .[1]' \
  ota_upstream.json ota_fork.json > ota_index.json.new &&
mv ota_index.json.new ota_index.json
```

Open Dimmer needs the fork's Zigbee firmware. Under the stock Zigbee firmware it is safe (the
wall switch works) but gets no Zigbee control or updates: convert the Zigbee side (step 1)
without re-pairing or resetting the module first.
