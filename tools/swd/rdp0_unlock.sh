#!/bin/bash
# Catch the PY32 (power-cycle VCC while this runs), keep debug alive in Stop, then program
# option bytes RDP=0xAA with Puya's own OPT flash algorithm. IRREVERSIBLE: the RDP 1->0
# change mass-erases the main flash.
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cd "$HERE/../../.."            # ~/projects/zdms16-py32: .venv and pack/ live there
"$HERE/catch_halt.sh" 90 || exit 1
.venv/bin/pyocd load -t py32f002ax5 --pack pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack -f 400000 \
  -O connect_mode=attach --base-address 0x1FFF0E80 "$HERE/ob_rdp0.bin" 2>&1 | grep -v -E "^\s*$" | tail -6
echo "--- after programming:"
.venv/bin/pyocd commander -t py32f002ax5 --pack pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack -f 400000 \
  -O connect_mode=attach -c "read32 0x1FFF0E80 16" -c "read32 0x40022020 4" -c "read32 0x08000000 16" 2>&1 \
  | grep -v -E "^\s*$" | tail -4
