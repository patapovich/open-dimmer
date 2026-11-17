#!/bin/bash
# Like dump_py32.sh, but without BOOT0: keeps trying to attach over SWD while the
# module is powered up. The stock firmware (WiFi build) waits 1.5 s in main() before
# its GPIO init takes PA13/PA14 away from SWD; an attach + halt inside that window
# stops it there. Start this, THEN connect the module's VCC.
# Usage: dump_py32_powerup.sh [out_dir] [seconds_to_keep_trying (default 60)]
OUT=${1:-$HOME/projects/zdms16-py32/dump-$(date +%Y%m%d-%H%M%S)}
TRY=${2:-60}
mkdir -p "$OUT"
END=$(( $(date +%s) + TRY ))
n=0
echo "trying for ${TRY}s - connect the module's VCC now"
while [ $(date +%s) -lt $END ]; do
  n=$((n + 1))
  if openocd -f interface/cmsis-dap.cfg \
      -c "transport select swd; adapter speed 400" \
      -c "swd newdap py32 cpu -expected-id 0" \
      -c "dap create py32.dap -chain-position py32.cpu" \
      -c "target create py32.cpu cortex_m -dap py32.dap" \
      -c "init" -c "halt 100" \
      -c "dump_image $OUT/mainflash_08000000_20KB.bin 0x08000000 0x5000" \
      -c "dump_image $OUT/systemrom_1FFF0000_3584B.bin 0x1FFF0000 0xE00" \
      -c "dump_image $OUT/uid_1FFF0E00_128B.bin 0x1FFF0E00 0x80" \
      -c "dump_image $OUT/optionbytes_1FFF0E80_128B.bin 0x1FFF0E80 0x80" \
      -c "dump_image $OUT/factoryconfig_1FFF0F00_128B.bin 0x1FFF0F00 0x80" \
      -c "shutdown" > "$OUT/openocd-last.log" 2>&1 \
     && [ -s "$OUT/mainflash_08000000_20KB.bin" ]; then
    echo "attached on try $n"
    grep -E "Info : |halted" "$OUT/openocd-last.log" | head -8
    ls -l "$OUT"; sha256sum "$OUT"/*.bin
    exit 0
  fi
  echo "$(date +%T.%N | cut -c1-12) try $n: $(grep -E 'Error|DPIDR|WAIT|FAULT|SWDIO' "$OUT/openocd-last.log" | tr '\n' ' ' | cut -c1-200)" >> "$OUT/attempts.log"
done
echo "no attach in ${TRY}s ($n tries); last log:"; tail -5 "$OUT/openocd-last.log"
echo "-> use BOOT0 (pin 5 to 3.3 V before power-up) and dump_py32.sh"
exit 1
