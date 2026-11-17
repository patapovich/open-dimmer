#!/bin/bash
# Deploy the newest module firmware built on the fork's home-dimmer_build to the named
# ZDMS16-2 modules: one at a time, Z2M image_block_response_delay 50 ms with the guard that
# puts 250 back. The ZTU then updates an own PY32 MCU that runs an older version (never a
# stock MCU). Local only: needs ~/projects/tzs-diag and ~/ha-claude (zdms16_ota_series.sh,
# zdms16_ota_guard.sh, .env).
# Usage: tools/ha/deploy_ota.sh <ieee> [<ieee>...]      (no default: always name the modules)
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: $0 <ieee> [<ieee>...]"; exit 2; }
ZTU_DIR=~/projects/tzs-diag
HA=~/ha-claude
TMP=$(mktemp -d)

git -C "$ZTU_DIR" fetch -q patapovich home-dimmer_build
C=$(git -C "$ZTU_DIR" log --format=%h -1 --grep="Update firmware files" patapovich/home-dimmer_build)
P=$(git -C "$ZTU_DIR" show --name-only --format= "$C" | grep "MODULE_AVATTO_ZDMS16_2/.*\.zigbee$" | grep -v "forced\|from_tuya")
F=$(basename "$P")
git -C "$ZTU_DIR" show "$C:$P" > "$TMP/$F"
python3 - "$TMP/$F" <<'PY'
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
found = None
pos = d.find(b"APDZ")
while pos >= 0:                       # the header whose size and CRC check out (the ZTU code
    start = pos - 0xC0                # also contains the magic as a constant)
    size, crc = struct.unpack_from("<II", d, pos + 4) if pos + 12 <= len(d) else (0, 0)
    if start >= 0 and 0 < size <= 12 * 1024 and start + size <= len(d):
        img = bytearray(d[start:start + size])
        img[0xC8:0xCC] = bytes(4)
        if zlib.crc32(img) == crc:
            found = d[pos + 12]
            break
    pos = d.find(b"APDZ", pos + 1)
v = found
print(f"image {sys.argv[1].rsplit('/', 1)[1]}: file version 0x{struct.unpack_from('<I', d, 14)[0]:08x}, "
      + (f"carries PY32 {v >> 6}.{(v >> 4) & 3}.{v & 15}" if v is not None else "NO valid PY32 image inside"))
PY
read -r -p "deploy to $* ? [y/N] " ok
[ "$ok" = "y" ] || { echo "not deployed"; exit 1; }

scp -q "$TMP/$F" root@homeassistant.local:/config/zigbee2mqtt/images/
# shellcheck disable=SC1091
source "$HA/.env"
curl -s -o /dev/null -X POST -H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json" \
    "$HA_URL/api/services/mqtt/publish" \
    -d '{"topic":"zigbee2mqtt/bridge/request/options","payload":"{\"options\":{\"ota\":{\"image_block_response_delay\":50}}}"}'
pairs=()
for ieee in "$@"; do pairs+=("$ieee:$ieee"); done
("$HA/zdms16_ota_series.sh" "$F" "${pairs[@]}" > "$TMP/series.log" 2>&1; echo "series exit $?" >> "$TMP/series.log") &
sleep 3
"$HA/zdms16_ota_guard.sh" "$TMP/series.log"
wait
grep -a -E "DONE|exit|STOP|FAIL" "$TMP/series.log" | grep -v "MQTT publish" | tail -"$(( $# + 2 ))"
echo "health (a minute after the MCU update): $HA/zdms16_health.py <ieee>"
