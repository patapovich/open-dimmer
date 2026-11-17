#!/bin/bash
# Keep trying to attach over SWD and HALT the PY32 (power-cycle its VCC meanwhile).
# On success the core is left halted (OpenOCD does not resume on shutdown), so a
# following pyOCD session with --connect attach finds it awake. DBGMCU_CR.DBG_STOP
# (0x40015804 bit 1) keeps the debug port alive in Stop mode until the next power-up.
# Usage: catch_halt.sh [seconds (default 90)]
TRY=${1:-90}; END=$(( $(date +%s) + TRY )); n=0
echo "trying for ${TRY}s - power-cycle the module's VCC now"
while [ $(date +%s) -lt $END ]; do
  n=$((n + 1))
  OUT=$(timeout 10 openocd -f interface/cmsis-dap.cfg -c "transport select swd; adapter speed 400" \
    -c "swd newdap py32 cpu -expected-id 0" -c "dap create py32.dap -chain-position py32.cpu" \
    -c "target create py32.cpu cortex_m -dap py32.dap" -c "init" -c "halt 100" -c "mww 0x40015804 0x2" -c "echo DBGMCU_CR=[read_memory 0x40015804 32 1]" \
    -c "echo PC=[reg pc]" -c "echo OPTR=[read_memory 0x40022020 32 1]" -c "shutdown" 2>&1)
  if echo "$OUT" | grep -q "OPTR="; then
    echo "halted on try $n"; echo "$OUT" | grep -E "DPIDR|PC=|OPTR=|DBGMCU" | head -4; exit 0
  fi
done
echo "no attach in ${TRY}s ($n tries)"; exit 1
