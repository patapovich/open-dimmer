# Installing Open Dimmer with a debug probe

The alternative to the [two-wire install](INSTALL.md): a probe on the MCU's SWD pads removes the
read protection and flashes Open Dimmer. Do [step 1 of INSTALL.md](INSTALL.md#1-zigbee-firmware-over-the-air)
(the Zigbee firmware) first.

> **Never on the mains.** The probe powers the module: all mains wires disconnected throughout.
> Once the read protection is removed the PY32 is blank until the install says **DONE**; never
> connect such a module to the mains, run the install again instead.

> **Settings go back to defaults** (min 20 %, max 100 %, switch type toggle, power-on off).

## What you need

- An SWD probe with a 3.3 V output. Tested: Raspberry Pi Pico with
  [debugprobe](https://github.com/raspberrypi/debugprobe) (`PROBE=cmsis-dap`, the default).
  Untested but known to pyOCD: ST-Link V2 (`PROBE=stlink`), J-Link (`PROBE=jlink`, needs a
  separate 3.3 V supply).
- A clip or pogo fixture for the four **CN2** pads (pitch most likely 2.0 mm); held wires make
  the attach unreliable. Bend the Zigbee module carefully aside to reach CN2. CN3 is the radio
  header: not for SWD.

  | CN2 pad | Signal | PY32F002A (TSSOP20) | Pico |
  |---|---|---|---|
  | 1 | SWD (SWDIO) | 2, PA13 | GP3 |
  | 2 | SWC (SWCLK) | 3, PA14 | GP2 |
  | 3 | GND | 1, VSS | GND |
  | 4 | VCC 3.3 V | 20, VCC | 3V3 OUT |

  ![PY32F002A pinout and the CN2 / CN3 pads (picture: divadiow, elektroda.com topic4066696)](img/dms16-pinout-elektroda.png)

  (Picture by divadiow, [elektroda.com topic4066696](https://www.elektroda.com/rtvforum/topic4066696.html),
  not covered by this repository's license; it shows the WiFi model, the PY32 side is the same.)
- Linux with Python 3 and pyOCD:

  ```
  sudo apt install python3-venv git curl
  mkdir -p ~/projects/zdms16-py32 && cd ~/projects/zdms16-py32
  git clone https://github.com/patapovich/zdms16-py32 fw
  git clone https://github.com/IOsetting/py32f0-template ../py32f0-template
  git -C ../py32f0-template checkout 009bdec7ed77c9b782a258c064c227e15ec19498
  python3 -m venv .venv && .venv/bin/pip install pyocd==0.46.1
  fw/tools/swd/make_opt_pack.sh "$PWD/../py32f0-template" "$PWD/pack"
  .venv/bin/pyocd list            # the probe must show up (else: udev rule)
  ```

## Install: one contact

```
cd fw
IMAGE=release FREQ=400000 tools/swd/install.sh
```

When it beeps, press the clip onto CN2 and hold it until **DONE** (10–20 s). The script attaches
at power-up, asks once, writes the option bytes (read protection off, which erases the stock
firmware; brown-out reset at 2.5 V), reloads them, writes bootloader and app, reads them back
and starts the firmware. On a module that already runs Open Dimmer it only re-flashes, keeping
the settings. `IMAGE=release:vX.Y.Z` picks a release; every download is checked against the
release's `SHA256SUMS`.

> The option reload path has not run on a stock module yet. If no `after reload: OPTR=` line
> appears 20 s after `option bytes: protection off ...`, lift the clip and press it on again
> (the script waits 60 s); never earlier. If it stops before **DONE**, run it again.

Then: probe off, module closed, mains on, **Reconfigure** in Zigbee2MQTT. If `mcu_firmware`
does not show "Open Dimmer", power-cycle the module, wait ~30 s and Reconfigure again. Set the
settings again (defaults above).

## By hand

1. **Read protection off** (erases the stock firmware): run `fw/tools/swd/rdp0_unlock.sh` and
   power-cycle the module's VCC while it tries to attach (90 s). Power-cycle again afterwards.
   "No ACK" means no contact. Alternative: BOOT0 (pin 5) to 3.3 V before power-up, removed
   before closing.
2. **Brown-out reset** (recommended):
   `.venv/bin/pyocd load -t py32f002ax5 --pack pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack -f 400000 -O connect_mode=attach --base-address 0x1FFF0E80 fw/tools/swd/ob_bor25.bin`,
   then power-cycle (OPTR reads `0x...f9aa`).
3. **Firmware:** `FREQ=400000 IMAGE=release fw/tools/swd/install.sh` (or `IMAGE=<file>` for a
   `zdms16_py32_full.bin`); it reads back and compares. `make flash-all` refuses an image over
   12 KB.

Never flash a `SIM=1` build to a module that goes back on the mains.

## The bootloader

The first 4 KB hold the bootloader that installs every update; it is written only over SWD and
stays. At power-on it installs the image the Zigbee firmware carries when its build is newer.
After three starts in a row that never confirmed themselves it listens 30 s before each start
and installs any image the Zigbee firmware carries. The app restarts itself when it hears
nothing from the Zigbee firmware for 30 min (lights off, switch idle). Health DP 136 is the
CRC-32 of the bootloader sector; the release notes give the expected value.
