# Installing on an AVATTO ZDMS16-2

The first installation is done once per module over SWD, with the module **off the mains**.
After that, every update goes over Zigbee.

> **Mains voltage.** The module's low-voltage side is not isolated from the mains. Disconnect
> all mains wires before you open it, and power it only from the debug probe's 3.3 V during the
> whole procedure. Never connect a probe, a computer or an earthed oscilloscope to a module that
> is connected to the mains.

> **No way back.** The stock PY32 firmware is read-protected (RDP level 1). Removing the
> protection erases it, and it cannot be read out beforehand. After this the module needs this
> firmware (or another one written for it).

## What you need

- A CMSIS-DAP debug probe. Used here: a Raspberry Pi Pico with the
  [debugprobe](https://github.com/raspberrypi/debugprobe) firmware.
- Four wires to the module's 4-pad header **CN2** (SWD, SWC, GND, VCC). Do not use CN3, which is
  the radio module header.
- Linux with OpenOCD 0.12 and Python 3. Tools are expected in `~/projects/zdms16-py32`; the
  scripts and the Makefile use these paths:

  ```
  mkdir -p ~/projects/zdms16-py32 && cd ~/projects/zdms16-py32
  git clone https://github.com/patapovich/zdms16-py32 fw
  git clone https://github.com/IOsetting/py32f0-template ~/projects/py32f0-template
  python3 -m venv .venv && .venv/bin/pip install pyocd        # used: pyOCD 0.46
  fw/tools/swd/make_opt_pack.sh                               # pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack
  ```

- The firmware: either build it (`make` in `fw`, needs `arm-none-eabi-gcc`) or take
  `zdms16_py32_full.bin` from a [release](https://github.com/patapovich/zdms16-py32/releases).

## 0. Zigbee side first

The module must run the Zigbee firmware that carries this MCU firmware: the `home-dimmer` build
of [patapovich/tuya-zigbee-switch](https://github.com/patapovich/tuya-zigbee-switch) (a fork of
[romasku/tuya-zigbee-switch](https://github.com/romasku/tuya-zigbee-switch); converting a stock
module over Zigbee is described there). Check in Zigbee2MQTT that the module works with the
stock MCU (`mcu_version` shows the stock version, for example 1.0.7) before going on.

## 1. Wiring

| Pico (debugprobe) | Module CN2 |
|---|---|
| GND | GND |
| 3V3 OUT | VCC |
| GP2 | SWC (PA14) |
| GP3 | SWD (PA13) |

Put the 3V3 wire on a contact you can open and close easily: step 2 needs a power cycle while
the script runs.

## 2. Remove the read protection (erases the stock firmware)

```
fw/tools/swd/rdp0_unlock.sh
```

The script keeps trying to attach for 90 s. **Power-cycle the module's VCC while it runs.** The
stock firmware takes the SWD pins over shortly after power-up, so the attach has to happen in
that window. Once the core is halted, the script writes option bytes with RDP = 0xAA
(`tools/swd/ob_rdp0.bin`: `aa f0 55 0f ff 00 00 ff ...`). The chip then erases its flash.
Power-cycle the module again afterwards.

If the attach never succeeds, check the wiring first ("No ACK" means no contact). As an
alternative, tie BOOT0 (PY32 pin 5) to 3.3 V before power-up: the ROM bootloader then runs
instead of the stock firmware (see `tools/swd/dump_py32.sh`).

## 3. Brown-out reset (recommended)

BOR at 2.5 V makes the MCU reset cleanly when the supply sags, instead of running on undefined:

```
cd ~/projects/zdms16-py32
.venv/bin/pyocd load -t py32f002ax5 --pack pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack -f 400000 \
    -O connect_mode=attach --base-address 0x1FFF0E80 fw/tools/swd/ob_bor25.bin
```

`ob_bor25.bin` is `aa f9 55 06 ff 00 00 ff ...` (RDP stays 0xAA, BOR on at 2.5 V). Power-cycle
the module afterwards. `read32 0x40022020` in `pyocd commander` shows OPTR `0x...f9aa`.

## 4. Flash the firmware

```
cd ~/projects/zdms16-py32/fw
make flash-all                      # builds and writes bootloader + app (full image)
```

or, with a release file:

```
cd ~/projects/zdms16-py32
.venv/bin/pyocd load -t py32f002ax5 --pack pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack -f 400000 \
    --format bin --base-address 0x08000000 zdms16_py32_full.bin
```

Never flash a `SIM=1` build to a module that will go back on the mains: it simulates the
zero cross and does not follow the real one.

## 5. Back in service

1. Power-cycle, disconnect the probe, close the module, connect it to the mains.
2. In Zigbee2MQTT, `mcu_version` now shows `2.x.y own`. Restart Zigbee2MQTT once so that it
   shows the settings that only this firmware has (device option `own_mcu_settings`, default
   auto).
3. Set the lamp up: `min_level` just above the level where the lamp still lights from dark,
   then `dimming_curve` (led for most LED lamps, led_deep or led_deeper if the dim end passes
   too quickly), and the switch type of the wall switch (momentary for push buttons).

## Updates

The Zigbee firmware carries the MCU firmware and updates the MCU when it runs an older version
of this firmware (never the stock firmware). Update the Zigbee firmware over the air as usual;
the MCU follows a few seconds after the module restarts, and the light comes back as it was.
