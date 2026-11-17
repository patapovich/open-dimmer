#!/bin/bash
# Make the pyOCD pack used here: Puya.PY32F0xx_DFP 1.1.7 (Misc/ of py32f0-template) with the
# PY32F002Ax5's option-byte flash algorithm (Flash/PY32F0xx_OPT.FLM) switched on, so pyOCD
# can write the option bytes (RDP removal, BOR).
# Usage: make_opt_pack.sh [py32f0-template dir] [output dir]
#        (defaults ~/projects/py32f0-template and ~/projects/zdms16-py32/pack)
set -euo pipefail
SRC=${1:-$HOME/projects/py32f0-template}
OUT=${2:-$HOME/projects/zdms16-py32/pack}
TMP=$(mktemp -d)
unzip -q "$SRC/Misc/Puya.PY32F0xx_DFP.1.1.7.pack" -d "$TMP"
python3 - "$TMP/Puya.PY32F0xx_DFP.pdsc" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
dev = t.index('<device Dname="PY32F002Ax5">')
end = t.index("</device>", dev)
old = '<algorithm name="Flash/PY32F0xx_OPT.FLM" start="0x1FFF3080" size="0x00000010" default="0"/>'
block = t[dev:end]
assert old in block, "option-byte algorithm line not found"
open(p, "w").write(t[:dev] + block.replace(old, old.replace('default="0"', 'default="1"')) + t[end:])
PY
mkdir -p "$OUT"
rm -f "$OUT/Puya.PY32F0xx_DFP.1.1.7-opt.pack"
(cd "$TMP" && zip -q -r "$OUT/Puya.PY32F0xx_DFP.1.1.7-opt.pack" .)
echo "$OUT/Puya.PY32F0xx_DFP.1.1.7-opt.pack"
