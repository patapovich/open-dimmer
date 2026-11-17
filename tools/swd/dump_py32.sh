#!/bin/bash
# Read the PY32F002A of a ZDMS16-2 over SWD with a Pico Debugprobe (CMSIS-DAP).
# Wiring: Pico GND -> GND, 3V3 OUT -> VCC, GP2 -> SWC (PA14), GP3 -> SWD (PA13),
# BOOT0 (pin 5) tied to 3V3 BEFORE power-up so the ROM bootloader runs and the
# stock firmware cannot take the SWD pins. Board OFF MAINS.
# Usage: dump_py32.sh [out_dir]   (default ~/projects/zdms16-py32/dump-<date>)
OUT=${1:-$HOME/projects/zdms16-py32/dump-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
openocd -f interface/cmsis-dap.cfg \
  -c "transport select swd; adapter speed 500" \
  -c "swd newdap py32 cpu -expected-id 0" \
  -c "dap create py32.dap -chain-position py32.cpu" \
  -c "target create py32.cpu cortex_m -dap py32.dap" \
  -c "init" \
  -c "halt" \
  -c "dump_image $OUT/mainflash_08000000_20KB.bin 0x08000000 0x5000" \
  -c "dump_image $OUT/systemrom_1FFF0000_3584B.bin 0x1FFF0000 0xE00" \
  -c "dump_image $OUT/uid_1FFF0E00_128B.bin 0x1FFF0E00 0x80" \
  -c "dump_image $OUT/optionbytes_1FFF0E80_128B.bin 0x1FFF0E80 0x80" \
  -c "dump_image $OUT/factoryconfig_1FFF0F00_128B.bin 0x1FFF0F00 0x80" \
  -c "shutdown" 2>&1 | grep -v -E "^$|Licensed|For bug|doxygen"
ls -l "$OUT"; sha256sum "$OUT"/*.bin
