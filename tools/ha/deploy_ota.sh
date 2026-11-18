#!/bin/bash
# Deploy the newest module firmware built on the fork's home-dimmer_build branch to the named
# ZDMS16-2 modules through Zigbee2MQTT, one module at a time. While the series runs, Z2M's
# ota.image_block_response_delay is 50 ms; 250 ms is put back on every exit path (and by the
# guard at the first BUFFER_FULL). On Ctrl-C the series is stopped first and 250 ms put back;
# a further Ctrl-C, TERM, HUP or a closed output pipe cannot cut that cleanup short. The ZTU then
# updates a PY32 MCU that runs an older version of Open Dimmer (never a stock MCU).
#
# Usage: tools/ha/deploy_ota.sh <ieee | friendly_name:ieee>...   (no default: always name the modules)
#   Z2M logs 'Finished update of ...' and '... failed' with the friendly name. For a bare IEEE the
#   name comes from the HA device registry: the device 'name' that Z2M's discovery keeps equal to
#   the friendly name (not name_by_user). The Z2M configuration is never read.
#
# Local only. Environment (all optional):
#   ZTU_DIR     checkout of the ZTU fork (default ~/projects/tzs-diag)
#   ZTU_REMOTE  its remote for github.com/patapovich/tuya-zigbee-switch (default patapovich)
#   HA_HELPERS  directory of the local helpers (default ~/ha):
#                 zdms16_ota_series.sh <image> <name:ieee>...  updates the modules one at a time
#                 zdms16_ota_guard.sh <series output> <pid>    puts 250 ms back at the first BUFFER_FULL
#   HA_ENV      file with HA_URL= and HA_TOKEN= (default $HA_HELPERS/.env)
#   HA_SSH      ssh target of the HA host whose /config holds zigbee2mqtt (default root@homeassistant.local)
#   HA_WS       websocket client for the registry lookup (default ha_ws.py beside this script)
# Exit status: the series' status (0 = all modules done); 3 when 250 ms could not be put back.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ZTU_DIR=${ZTU_DIR:-$HOME/projects/tzs-diag}
ZTU_REMOTE=${ZTU_REMOTE:-patapovich}
HA_HELPERS=${HA_HELPERS:-$HOME/ha}
HA_ENV=${HA_ENV:-$HA_HELPERS/.env}
HA_SSH=${HA_SSH:-root@homeassistant.local}
HA_WS=${HA_WS:-$HERE/ha_ws.py}
export HA_ENV HA_SSH
SERIES=$HA_HELPERS/zdms16_ota_series.sh
GUARD=$HA_HELPERS/zdms16_ota_guard.sh

die() { echo "$(basename "$0"): $*" >&2; exit 1; }
ask() { local ok; read -r -p "$1 [y/N] " ok || ok=""; [ "$ok" = "y" ]; }
ver() { printf '0x%08x' "$1"; }

[ $# -ge 1 ] || { echo "usage: $0 <ieee | friendly_name:ieee>..."; exit 2; }
ieees=(); given=()
for a in "$@"; do
    ieee=${a##*:}; ieee=${ieee,,}
    [[ $ieee =~ ^0x[0-9a-f]{16}$ ]] || { echo "not an IEEE address in '$a' (want <ieee> or <friendly_name>:<ieee>)"; exit 2; }
    name=""; [[ $a == *:* ]] && name=${a%:*}
    ieees+=("$ieee"); given+=("$name")
done
[ -x "$SERIES" ] && [ -x "$GUARD" ] || die "no executable zdms16_ota_series.sh and zdms16_ota_guard.sh in $HA_HELPERS (set HA_HELPERS)"
[ -r "$HA_ENV" ] || die "cannot read $HA_ENV (set HA_ENV or HA_HELPERS)"
# shellcheck disable=SC1090
source "$HA_ENV"
[ -n "${HA_URL:-}" ] && [ -n "${HA_TOKEN:-}" ] || die "HA_URL or HA_TOKEN missing in $HA_ENV"
RUNNING=$(pgrep -af zdms16_ota_series.sh || true)
[ -z "$RUNNING" ] || die "an OTA series is running already (one module at a time): $RUNNING"
TMP=$(mktemp -d)

# --- the image: newest build of the fork, a valid OTA file that carries a valid PY32 image
git -C "$ZTU_DIR" fetch -q "$ZTU_REMOTE" home-dimmer_build
C=$(git -C "$ZTU_DIR" log --format=%h -1 --grep="Update firmware files" "$ZTU_REMOTE/home-dimmer_build")
[ -n "$C" ] || die "no 'Update firmware files' commit on $ZTU_REMOTE/home-dimmer_build"
P=$(git -C "$ZTU_DIR" show --name-only --format= "$C" | grep "MODULE_AVATTO_ZDMS16_2/.*\.zigbee$" | grep -v "forced\|from_tuya" || true)
[ -n "$P" ] && [ "$(printf '%s\n' "$P" | wc -l)" -eq 1 ] || die "expected one ZDMS16-2 .zigbee image in $C, found: ${P:-none}"
F=$(basename "$P")
git -C "$ZTU_DIR" show "$C:$P" > "$TMP/$F"
IMGVER=$(python3 - "$TMP/$F" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
name = path.rsplit("/", 1)[1]
d = open(path, "rb").read()
if len(d) < 56 or struct.unpack_from("<I", d, 0)[0] != 0x0BEEF11E or struct.unpack_from("<I", d, 52)[0] != len(d):
    sys.exit(f"{name}: not a Zigbee OTA file (magic or total size wrong)")
fver = struct.unpack_from("<I", d, 14)[0]
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
if found is None:
    sys.exit(f"{name}: file version 0x{fver:08x}, NO valid PY32 image inside")
v = found
print(f"image {name}: file version 0x{fver:08x}, carries PY32 {v >> 6}.{(v >> 4) & 3}.{v & 15}", file=sys.stderr)
print(fver)
PY
) || die "refusing to deploy $F (from $C)"

# --- the modules: Z2M friendly name and installed file version from the HA registries (read only)
REG=1
python3 "$HA_WS" --json '{"type":"config/device_registry/list"}' '{"type":"config/entity_registry/list"}' \
    '{"type":"get_states"}' > "$TMP/registry.json" 2> "$TMP/registry.err" || REG=0
if [ "$REG" = 1 ]; then
    python3 - "$TMP/registry.json" "${ieees[@]}" > "$TMP/modules.txt" <<'PY' || REG=0
import json, sys
devices, entities, states = (json.loads(line) for line in open(sys.argv[1]).read().splitlines()[:3])
state = {s["entity_id"]: s for s in states}
for ieee in sys.argv[2:]:
    dev = next((d for d in devices if ["mqtt", "zigbee2mqtt_" + ieee] in [list(i) for i in d.get("identifiers", [])]), None)
    name = installed = ""
    if dev is not None:
        name = dev.get("name") or ""            # set by Z2M discovery = friendly name; not name_by_user
        for e in entities:
            if e.get("device_id") == dev["id"] and e["entity_id"].startswith("update."):
                v = str((state.get(e["entity_id"]) or {}).get("attributes", {}).get("installed_version"))
                if v.isdigit():
                    installed = v
    print(f"{ieee}\x1f{name}\x1f{installed}")
PY
fi
[ "$REG" = 1 ] || { echo "HA registry lookup failed:"; sed 's/^/  /' "$TMP/registry.err"; }
pairs=(); labels=(); notnewer=()
for i in "${!ieees[@]}"; do
    ieee=${ieees[$i]}; name=${given[$i]}; regname=""; installed=""
    if [ "$REG" = 1 ]; then
        IFS=$'\x1f' read -r _ regname installed < <(grep -a "^$ieee"$'\x1f' "$TMP/modules.txt") || true
    fi
    if [ -z "$name" ]; then
        name=$regname
        [ -n "$name" ] || die "no Z2M friendly name for $ieee in the HA device registry: pass <friendly_name>:$ieee"
    elif [ -n "$regname" ] && [ "$regname" != "$name" ]; then
        echo "note: HA knows $ieee as '$regname', using the given '$name'"
    fi
    if [ -z "$installed" ]; then
        state="installed version unknown"
    elif [ "$installed" -lt "$IMGVER" ]; then
        state="installed $(ver "$installed") -> $(ver "$IMGVER")"
    else
        state="installed $(ver "$installed"), image $(ver "$IMGVER"): $([ "$installed" -gt "$IMGVER" ] && echo DOWNGRADE || echo "SAME version")"
        notnewer+=("$name")
    fi
    pairs+=("$name:$ieee"); labels+=("$name")
    echo "  $name ($ieee): $state"
done

# --- the HA host: ssh works and the clocks agree (the Z2M log is read by timestamp)
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=10 -o ServerAliveCountMax=3)
R=$(ssh -n "${SSH_OPTS[@]}" "$HA_SSH" date +%s) || die "ssh $HA_SSH failed"
[[ $R =~ ^[0-9]+$ ]] || die "ssh $HA_SSH date: unexpected answer '$R'"
SKEW=$(( R - $(date +%s) )); SKEW=${SKEW#-}
[ "$SKEW" -le 8 ] || die "the clocks here and on $HA_SSH differ by $SKEW s (the Z2M log is read by timestamp)"

if [ ${#notnewer[@]} -gt 0 ]; then
    ask "not newer than the installed firmware on ${notnewer[*]}: deploy there anyway?" || { echo "not deployed"; exit 1; }
fi
ask "deploy $F to ${labels[*]} ?" || { echo "not deployed"; exit 1; }

ha_post() {  # <api path> <json body>: retried; non-zero (curl prints why) unless HA accepted it
    curl -fsS --retry 3 --retry-connrefused --retry-delay 2 --max-time 20 -o /dev/null -X POST \
        -H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json" "$HA_URL$1" -d "$2"
}
set_delay() {
    ha_post /api/services/mqtt/publish \
        "{\"topic\":\"zigbee2mqtt/bridge/request/options\",\"payload\":\"{\\\"options\\\":{\\\"ota\\\":{\\\"image_block_response_delay\\\":$1}}}\"}"
}
SP=""; GP=""; TP=""; DELAY_SET=0; GSHOWN=0
# shellcheck disable=SC2329  # used by cleanup (EXIT trap)
say() { echo "$@" 2>/dev/null || { exec 1>&2; echo "$@"; }; }   # output pipe gone (| tee killed): stderr
# shellcheck disable=SC2329  # runs from the EXIT trap
cleanup() {
    local rc=$? stopping=0
    set +e
    trap - EXIT
    trap '' INT TERM HUP PIPE   # a second Ctrl-C or a closed output pipe must not cut this short
    if [ -n "$SP" ] && kill -0 "$SP" 2>/dev/null; then
        kill -TERM "$SP"; pkill -TERM -P "$SP"; stopping=1   # first, before any output
        [ "$rc" -ne 0 ] || rc=1
        say "stopping the OTA series (pid $SP), its exit clears the OTA index; Ctrl-C is ignored until this cleanup is done"
    fi
    if [ "$DELAY_SET" = 1 ]; then   # before the wait for the series and while the guard still runs
        if set_delay 250; then
            say "image_block_response_delay 250 sent (HA accepted the publish)"
        else
            say "!!! FAILED to put image_block_response_delay back to 250: set it in Z2M by hand (Settings, OTA)"
            rc=3
        fi
    fi
    if [ "$stopping" = 1 ]; then
        for _ in $(seq 45); do kill -0 "$SP" 2>/dev/null || break; sleep 1; done
        kill -0 "$SP" 2>/dev/null &&
            say "!!! the OTA series (pid $SP) is still running: kill it, then write [] to /config/zigbee2mqtt/local_ota_index.json"
    fi
    for p in $GP $TP; do pkill -TERM -P "$p"; kill -TERM "$p"; done 2>/dev/null
    [ -n "$TP" ] && GSHOWN=$(wc -l < "$TMP/guard.log")
    [ -e "$TMP/guard.log" ] && tail -n +$(( GSHOWN + 1 )) "$TMP/guard.log"   # what the live view missed
    [ -e "$TMP/series.log" ] && say "series output: $TMP/series.log"
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

scp -q "${SSH_OPTS[@]}" "$TMP/$F" "$HA_SSH:/config/zigbee2mqtt/images/" || die "copying $F to $HA_SSH failed"
DELAY_SET=1
set_delay 50 || die "setting image_block_response_delay 50 failed: not deployed"
: > "$TMP/guard.log"
"$SERIES" "$F" "${pairs[@]}" > "$TMP/series.log" 2>&1 &
SP=$!
"$GUARD" "$TMP/series.log" "$SP" > >(sed -u 's/^/guard: /' > "$TMP/guard.log") 2>&1 &
GP=$!
tail -q -n +1 -s 2 -f --pid="$SP" "$TMP/series.log" "$TMP/guard.log" &
TP=$!
RC=0
wait "$SP" || RC=$?
SP=""
wait "$TP" 2>/dev/null || true
TP=""; GSHOWN=$(wc -l < "$TMP/guard.log")
echo "--- series exit $RC"
grep -a -E "^(=== |DONE|FAILED|NO START|NO BLOCKS|STALLED|TIMEOUT|STOPPED|ALL DONE)" "$TMP/series.log" || true
if [ "$RC" = 0 ] && [ -x "$HA_HELPERS/zdms16_health.py" ]; then
    echo "health (a minute after the MCU update): $HA_HELPERS/zdms16_health.py <ieee>"
fi
exit "$RC"
