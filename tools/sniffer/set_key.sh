#!/usr/bin/env bash
# Writes the Zigbee network key into a Wireshark key table ($KEYDIR/zigbee_pc_keys, root:$GROUP
# 0640, directory 0750), so that tshark (zbpcap.py) can decrypt the sniffer's captures.
# Run as root on the sniffer host. The key is read from the terminal without echo (or from
# stdin when that is not a terminal) and never printed. Both byte orders are written: Wireshark
# tries every key in the table.
# Accepts exactly one of these forms, anything else is refused and nothing is written:
#   - Zigbee2MQTT's network_key: 16 decimal numbers 0-255 split by commas, with or without the
#     brackets ([1, 3, ...] or 1, 3, ...), or as 16 YAML list lines ('- 1', one per line);
#   - 32 hex digits, in one piece or split by ':', '-' or spaces into whole bytes (00:11:..,
#     00 11 ..), each part with or without a 0x prefix.
set -euo pipefail
KEYDIR="${KEYDIR:-/etc/zbsniff/wireshark}"
GROUP="${GROUP:-zbsniff}"
if [ -t 0 ]; then
  read -rsp "Zigbee network key (one line): " key
  echo
else
  key=$(cat)
fi
hex=$(printf '%s' "$key" | python3 -I -c '
import re, sys
k = sys.stdin.read().strip()
lines = [l.strip() for l in k.splitlines() if l.strip()]
if len(lines) > 1 or k.startswith("-"):    # YAML list lines: "- 12", one per line
    m = [re.fullmatch(r"-\s*([0-9]{1,3})", l) for l in lines]
    if len(m) != 16 or not all(m):
        sys.exit(1)
    n = [int(x.group(1)) for x in m]
elif "," in k:                             # Zigbee2MQTT network_key: [1, 3, ...] or 1, 3, ...
    body = k[1:-1] if k.startswith("[") and k.endswith("]") else k
    items = [x.strip() for x in body.split(",")]
    if len(items) != 16 or not all(re.fullmatch(r"[0-9]{1,3}", x) for x in items):
        sys.exit(1)
    n = [int(x) for x in items]
else:                                      # hex: whole bytes split by ":", "-" or spaces, 0x allowed
    parts = [p[2:] if p[:2].lower() == "0x" else p for p in re.split(r"[\s:-]+", k)]
    compact = "".join(parts)
    if any(len(p) % 2 for p in parts) or not re.fullmatch(r"[0-9a-fA-F]{32}", compact):
        sys.exit(1)
    n = list(bytes.fromhex(compact))
if not all(0 <= x <= 255 for x in n):
    sys.exit(1)
print(bytes(n).hex())
') || { unset key; echo "not a 16-byte key (16 decimal numbers 0-255 split by commas or as '- 1' lines, or 32 hex digits): nothing written" >&2; exit 1; }
unset key
umask 027
install -d -m 750 -g "$GROUP" "$KEYDIR"
tmp=$(mktemp "$KEYDIR/.zigbee_pc_keys.XXXXXX")
{ printf '"%s","Normal","zbsniff"\n' "$hex"; printf '"%s","Reverse","zbsniff-reverse"\n' "$hex"; } > "$tmp"
unset hex
chgrp "$GROUP" "$tmp"
chmod 640 "$tmp"
mv -f "$tmp" "$KEYDIR/zigbee_pc_keys"
echo "written: $KEYDIR/zigbee_pc_keys"
