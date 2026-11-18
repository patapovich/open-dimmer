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

- A debug probe with SWD and a 3.3 V output to power the module. Tested: a Raspberry Pi Pico /
  RP2040 board with the [debugprobe](https://github.com/raspberrypi/debugprobe) firmware.
  Should work too (pyOCD and OpenOCD know them; not tried here):

  | Probe | `PROBE=` | Powers the module |
  |---|---|---|
  | Pico / RP2040 with debugprobe (CMSIS-DAP) | `cmsis-dap` (default) | yes, 3V3 OUT |
  | ST-Link V2 (also the cheap clones) | `stlink` | yes, 3.3V pin |
  | SEGGER J-Link | `jlink` | not by default: use a separate 3.3 V supply |
  | other CMSIS-DAP probes (DAPLink, ...) | `cmsis-dap` | depends on the probe |

- A way to hold four contacts on the module's **CN2** pads (SWD, SWC, GND, VCC; not CN3,
  which is the radio header) without your hands. The CN2 pads are closer than 2.54 mm, most likely
  **2.0 mm** pitch (to be confirmed: first to last pad centre ≈ 6.0 mm at 2.0 mm, ≈ 3.8 mm at
  1.27 mm). Options:
  - a single-row **4-pin pogo-pin clip / PCB clamp fixture in 2.0 mm pitch** (search "PCB clamp
    fixture 2.0mm 4P pogo pin"; the common 2.54 mm ones do not fit), if the pads are close
    enough to the board edge for its jaws;
  - otherwise a 2.0 mm 4-pin **spring-loaded (pogo) connector** pressed down with a small clamp or
    a printed jig.
  Holding wires by hand works but makes the attach unreliable.
- Linux with OpenOCD (0.12 used) and Python 3, plus the tools the scripts call:

  ```
  sudo apt install openocd python3-venv git zip unzip curl
  sudo apt install make gcc-arm-none-eabi libnewlib-arm-none-eabi   # only to build it yourself
  ```

  A work directory holds the clone (it must be called `fw`), the Python environment `.venv` and
  the option-byte pack `pack/`; the SDK sits beside it. The SDK commit is the one CI builds
  with, and pyOCD is the version every step here was run with:

  ```
  mkdir -p ~/projects/zdms16-py32 && cd ~/projects/zdms16-py32       # the work directory
  git clone https://github.com/patapovich/zdms16-py32 fw
  git clone https://github.com/IOsetting/py32f0-template ../py32f0-template
  git -C ../py32f0-template checkout 009bdec7ed77c9b782a258c064c227e15ec19498
  python3 -m venv .venv && .venv/bin/pip install pyocd==0.46.1
  fw/tools/swd/make_opt_pack.sh "$PWD/../py32f0-template" "$PWD/pack"
  .venv/bin/pyocd list                                               # the probe must show up
  ```

  `tools/swd/install.sh` and `rdp0_unlock.sh` find `.venv` and `pack/` next to `fw` wherever the
  work directory is. The Makefile defaults to the paths above (`~/projects/zdms16-py32` and
  `~/projects/py32f0-template`); elsewhere give them to `make`:

  ```
  make SDK=<template>/Libraries PYOCD=<work>/.venv/bin/pyocd \
       PACK=<work>/pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack ...
  ```

  If `pyocd list` shows no probe or a permission error, the probe needs a udev rule (the
  openocd package brings rules for the common ones). The commands below run in the work
  directory or in `fw`.

## 0. Zigbee side first

The module must run the Zigbee firmware that carries this MCU firmware: the ZDMS16-2 build of
the fork [patapovich/tuya-zigbee-switch](https://github.com/patapovich/tuya-zigbee-switch)
(branch `home-dimmer`, built on `home-dimmer_build`), based on
[romasku/tuya-zigbee-switch](https://github.com/romasku/tuya-zigbee-switch). It is installed
over the air from the stock firmware, no opening needed:

1. Follow the upstream guide for Zigbee2MQTT:
   [conversion from the stock firmware](https://github.com/romasku/tuya-zigbee-switch/blob/main/docs/updating.md#conversion)
   and [external converters](https://github.com/romasku/tuya-zigbee-switch/blob/main/docs/updating.md#quirks--converters--extensions),
   but take the fork's files (only the fork has the ZDMS16-2 with the MCU firmware):
   - OTA index (`ota.zigbee_ota_override_index_location`):
     `https://raw.githubusercontent.com/patapovich/tuya-zigbee-switch/home-dimmer_build/zigbee2mqtt/ota/index_router.json`.
     It lists only the ZDMS16-2 (the update and the conversion from stock), and Zigbee2MQTT takes
     only one override index. If other devices of yours run tuya-zigbee-switch firmware, this
     index alone stops their updates and conversions. Merge upstream's index and the fork's into a
     local file instead: run this in the Zigbee2MQTT data directory (next to
     `configuration.yaml`), set the option to `index_router.json`, and run it again to pick up new
     releases. Upstream entries for the fork's devices are dropped, all others are kept.

     ```
     jq -s '(.[1] | map(.manufacturerName[]?)) as $m
            | [.[0][] | select(any(.manufacturerName[]?; IN($m[])) | not)] + .[1]' \
       <(curl -fsSL https://raw.githubusercontent.com/romasku/tuya-zigbee-switch/refs/heads/main/zigbee2mqtt/ota/index_router.json) \
       <(curl -fsSL https://raw.githubusercontent.com/patapovich/tuya-zigbee-switch/refs/heads/home-dimmer_build/zigbee2mqtt/ota/index_router.json) \
       > index_router.json
     ```

   - converters (`switch_custom.js`, and `tuya_with_ota.js` for the stock module):
     [zigbee2mqtt/converters](https://github.com/patapovich/tuya-zigbee-switch/tree/home-dimmer_build/zigbee2mqtt/converters)
   - images, if you install them by hand:
     [bin/router/MODULE_AVATTO_ZDMS16_2](https://github.com/patapovich/tuya-zigbee-switch/tree/home-dimmer_build/bin/router/MODULE_AVATTO_ZDMS16_2)
     (`*-from_tuya.zigbee` converts a stock module, the plain `.zigbee` updates a converted one)
2. Reconfigure the device in Zigbee2MQTT after the conversion, and check that it works with the
   stock MCU: `mcu_version` shows the stock version (for example 1.0.7). Only then open the
   module for the MCU part below.

## 1. Quick install: one contact

| Pico (debugprobe) | Module CN2 |
|---|---|
| GND | GND |
| 3V3 OUT | VCC |
| GP2 | SWC (PA14) |
| GP3 | SWD (PA13) |

With the module off the mains and the clip wired to the probe:

```
cd fw
IMAGE=release FREQ=400000 tools/swd/install.sh     # or build first and leave IMAGE out
```

When it beeps and says so, press the clip onto CN2 and hold it until **DONE** (about
10–20 s). In that one contact the script:

1. attaches right at power-up (the stock firmware takes the SWD pins a moment later),
2. asks once, then writes the option bytes in one go: read protection off (this **erases
   the stock firmware**) and brown-out reset at 2.5 V,
3. reloads the option bytes without a power cycle (FLASH_CR OBL_LAUNCH). If the chip does not
   come back, it beeps and asks you to lift the clip and press it again,
4. writes bootloader + app, reads it back and compares, and starts the firmware.

On a module that already runs this firmware it skips steps 2–3 and just re-flashes. A SIM
(bench) build is refused. Other probes: `PROBE=stlink` or `PROBE=jlink`; if the SWD link is
unreliable, `FREQ=1000000`.

> The one-contact path with the option reload has not been run on a stock module yet (the
> first module here was unlocked with the separate steps below). If anything is off, the
> script stops with a message; the steps by hand below always work.

## 2. Back in service

1. Power-cycle, disconnect the probe, close the module, connect it to the mains.
2. In Zigbee2MQTT, `mcu_version` now shows `2.x.y own`. Restart Zigbee2MQTT once so that it
   shows the settings that only this firmware has (device option `own_mcu_settings`, default
   auto).
3. Set the lamp up: `min_level` just above the level where the lamp still lights from dark,
   then `dimming_curve` (led_soft is the default and suits most LED lamps; led, led_deep or
   led_deeper if the dim end passes too quickly), and the switch type of the wall switch
   (momentary for push buttons).

## By hand (the same steps one by one)

### Wiring

| Pico (debugprobe) | Module CN2 |
|---|---|
| GND | GND |
| 3V3 OUT | VCC |
| GP2 | SWC (PA14) |
| GP3 | SWD (PA13) |

Put the 3V3 wire on a contact you can open and close easily: the next step needs a power
cycle while the script runs.

### Remove the read protection (erases the stock firmware)

```
fw/tools/swd/rdp0_unlock.sh          # in the work directory
```

The script keeps trying to attach for 90 s. **Power-cycle the module's VCC while it runs.** The
stock firmware takes the SWD pins over shortly after power-up, so the attach has to happen in
that window. Once the core is halted, the script writes option bytes with RDP = 0xAA
(`tools/swd/ob_rdp0.bin`: `aa f0 55 0f ff 00 00 ff ...`). The chip then erases its flash.
Power-cycle the module again afterwards.

If the attach never succeeds, check the wiring first ("No ACK" means no contact). As an
alternative, tie BOOT0 (PY32 pin 5) to 3.3 V before power-up: the ROM bootloader then runs
instead of the stock firmware (see `tools/swd/dump_py32.sh`).

### Brown-out reset (recommended)

BOR at 2.5 V makes the MCU reset cleanly when the supply sags, instead of running on undefined:

```
.venv/bin/pyocd load -t py32f002ax5 --pack pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack -f 400000 \
    -O connect_mode=attach --base-address 0x1FFF0E80 fw/tools/swd/ob_bor25.bin
```

`ob_bor25.bin` is `aa f9 55 06 ff 00 00 ff ...` (RDP stays 0xAA, BOR on at 2.5 V). Power-cycle
the module afterwards. `read32 0x40022020` in `pyocd commander` shows OPTR `0x...f9aa`.

### Flash the firmware

```
cd fw
make flash-all                      # builds and writes bootloader + app (full image)
```

or, with a release file
(`https://github.com/patapovich/zdms16-py32/releases/latest/download/zdms16_py32_full.bin`),
in the work directory; the first command stops the firmware's watchdog while the core is halted
(as `make flash-all` and `install.sh` do), which a re-flash of a running module needs:

```
.venv/bin/pyocd commander -t py32f002ax5 --pack pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack -f 400000 \
    -O connect_mode=attach -c "write32 0x40015808 0x1000"
.venv/bin/pyocd load -t py32f002ax5 --pack pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack -f 400000 \
    --format bin --base-address 0x08000000 zdms16_py32_full.bin
```

`FREQ=400000 IMAGE=<file> fw/tools/swd/install.sh` does the same and also reads the flash back
and compares.

Never flash a `SIM=1` build to a module that will go back on the mains: it simulates the
zero cross and does not follow the real one.

## Updates

The Zigbee firmware carries the MCU firmware and updates the MCU when it runs an older version
of this firmware (never the stock firmware). Update the Zigbee firmware over the air as usual;
the MCU follows a few seconds after the module restarts, and the light comes back as it was.
