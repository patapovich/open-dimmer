#!/bin/bash
# Update ZDMS16-2 modules one at a time through Zigbee2MQTT with one OTA image (normally the
# newest build of the ZTU fork's home-dimmer branch). deploy_ota.sh runs this; it copies the image
# and sets Z2M's block delay first. The image is offered through Z2M's local OTA index
# (force:true) only until the module's OTA has started.
# A stock conversion (deploy_ota.sh STOCK=1) sets the index entry through the env below:
# modelId TS0601, the stock manufacturer names, no force (the -from_tuya image is 0xFFFFFFFF).
# Usage: zdms16_ota_series.sh <image file name in /config/zigbee2mqtt/images> <friendly:ieee>...
#   friendly = the Z2M friendly name: Z2M logs 'Finished update of ...' and '... failed' with it
#   (zigbee-herdsman logs the IEEE). Both are matched; a module whose names do not match is
#   caught by the stall timeout.
# Stops at the first module that does not finish (exit 1: FAILED, NO START, NO BLOCKS, STALLED,
# TIMEOUT; STOPPED, a signal: exit 130 on INT (Ctrl-C), 143 on TERM or HUP); exit 0 after ALL
# DONE; exit 2 for bad arguments or settings. The OTA index (/config/zigbee2mqtt/
# local_ota_index.json, Z2M's "OTA index override file name") is rewritten for each module and
# cleared ([]) on every exit.
# Env: HA_ENV (file with HA_URL= and HA_TOKEN=; default $HA_HELPERS/.env, or ~/ha/.env when
#        HA_HELPERS is not set),
#      HA_SSH (ssh target of the HA host whose /config holds zigbee2mqtt, default
#        root@homeassistant.local),
#      OTA_STALL_S (no new progress line for this long = stalled, default 300),
#      OTA_CAP_S (longest download of one module, default 2700; 250 ms blocks take ~17 min),
#      OTA_INDEX_MODEL (index modelId, default TS0601-ZDMS16-2),
#      OTA_INDEX_MANUF (index manufacturerName list, comma-separated, default jtbgusdc),
#      OTA_INDEX_FORCE (1 = force:true, default 1).
# The Z2M log is read by full timestamp ('YYYY-MM-DD HH:MM:SS', midnight-safe); the clocks of
# this machine and the HA host must agree (deploy_ota.sh checks that). Every ssh call ends within
# 30 s (a dead connection is a failed read), so the stall and total timeouts always fire.
set -u
HA_ENV=${HA_ENV:-${HA_HELPERS:-$HOME/ha}/.env}
# shellcheck disable=SC1090
source "$HA_ENV" && [ -n "${HA_URL:-}" ] && [ -n "${HA_TOKEN:-}" ] ||
  { echo "cannot read HA_URL and HA_TOKEN from $HA_ENV (set HA_ENV)"; exit 2; }
HA_SSH=${HA_SSH:-root@homeassistant.local}
SSH=(timeout --foreground -k 5 30 ssh -n -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 "$HA_SSH")
STALL=${OTA_STALL_S:-300}
CAP=${OTA_CAP_S:-2700}
[ $# -ge 2 ] || { echo "usage: zdms16_ota_series.sh <image> <friendly:ieee>..."; exit 2; }
IMG=$1; shift
pub() {  # <topic> <payload>: MQTT publish through HA, retried; loud and non-zero on failure
  curl -fsS --retry 3 --retry-connrefused --retry-delay 2 --max-time 20 -o /dev/null -X POST \
    -H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json" "$HA_URL/api/services/mqtt/publish" \
    -d "$(python3 -c 'import json,sys; print(json.dumps({"topic":sys.argv[1],"payload":sys.argv[2]}))' "$1" "$2")" \
    || { echo "MQTT publish to $1 FAILED $(date +%T)"; return 1; }
}
# Z2M log lines from the full timestamp $1 on (lines without a timestamp go with the line before)
# shellcheck disable=SC2029
# (read whole before grep -q sees it, so an early grep exit is no ssh failure)
log() {
  local out
  out=$("${SSH[@]}" "L=\$(ls -1 /config/zigbee2mqtt/log | tail -1); awk -v t='[$1' 'substr(\$0, 1, 1) == \"[\" { ok = substr(\$0, 1, length(t)) >= t } ok' /config/zigbee2mqtt/log/\$L/log.log") \
    || echo "(reading the Z2M log failed $(date +%T))" >&2
  printf '%s\n' "$out"
}
since() { date -d "@$(( $(date +%s) - 10 ))" '+%F %T'; }   # now - 10 s: a few seconds of clock skew is fine
ere() { printf '%s' "$1" | sed 's/[][\.*^$+?(){}|]/\\&/g'; }
INDEX_B64=$(python3 -I -c 'import base64, json, sys
e = {"url": "images/" + sys.argv[1], "modelId": sys.argv[2], "manufacturerName": [m for m in sys.argv[3].split(",") if m]}
if sys.argv[4] == "1":
    e["force"] = True
print(base64.b64encode((json.dumps([e], indent=4) + "\n").encode()).decode())' \
  "$IMG" "${OTA_INDEX_MODEL:-TS0601-ZDMS16-2}" "${OTA_INDEX_MANUF:-jtbgusdc}" "${OTA_INDEX_FORCE:-1}") || { echo "bad index settings"; exit 2; }
# shellcheck disable=SC2029  # $INDEX_B64 (base64 only) expands here on purpose
set_index() { "${SSH[@]}" "echo $INDEX_B64 | base64 -d > /config/zigbee2mqtt/local_ota_index.json"; }
clear_index() { "${SSH[@]}" 'echo "[]" > /config/zigbee2mqtt/local_ota_index.json' || echo "!!! clearing the OTA index FAILED: write [] to /config/zigbee2mqtt/local_ota_index.json by hand"; }
trap clear_index EXIT
trap 'echo "STOPPED (signal) $(date +%T)"; exit 143' TERM HUP
trap 'echo "STOPPED (interrupt) $(date +%T)"; exit 130' INT

for PAIR in "$@"; do
  DEV=${PAIR%:*}; IEEE=${PAIR##*:}; IEEE=${IEEE,,}
  [[ $PAIR == *:* && -n $DEV && $IEEE =~ ^0x[0-9a-f]{16}$ ]] || { echo "bad argument '$PAIR' (want friendly:ieee)"; exit 2; }
  D=$(ere "$DEV")
  FAIL="(OTA update|Update|Downgrade) of '?($D|$IEEE)'? failed|Device '?$IEEE'? did not (start|finish)|Device '?($D|$IEEE)'? does not support OTA"
  OK_Z2M="Finished update of '$D'|Device '$D' was OTA updated from"
  OK_ZH="Update of '?$IEEE'? successful"
  echo "=== $DEV ($IEEE) $(date +%T)"
  T=$(since)
  set_index || { echo "FAILED $DEV: setting the OTA index failed"; exit 1; }
  pub zigbee2mqtt/bridge/request/device/ota_update/update "{\"id\":\"$IEEE\"}" || exit 1
  END=$(( $(date +%s) + 90 ))
  until log "$T" | grep -qE "OTA update of '$IEEE' (estimated|at)|$FAIL"; do
    sleep 2
    if [ "$(date +%s)" -gt "$END" ]; then
      clear_index; echo "NO START $DEV"; log "$T" | grep -E "'$D'|$IEEE" | tail -3 | cut -c1-200; exit 1
    fi
  done
  clear_index; echo "index cleared $(date +%T)"
  if log "$T" | grep -qE "$FAIL"; then echo "FAILED $DEV"; log "$T" | grep -E "$FAIL" | tail -2 | cut -c1-220; exit 1; fi
  # first block request within 60 s, or the device refused the image
  END=$(( $(date +%s) + 75 ))
  until log "$T" | grep -qE "OTA update of '$IEEE' at [0-9.]+%|$OK_ZH"; do
    sleep 5
    if log "$T" | grep -qE "$FAIL"; then echo "FAILED $DEV"; log "$T" | grep -E "$FAIL" | tail -2 | cut -c1-220; exit 1; fi
    [ "$(date +%s)" -gt "$END" ] && { echo "NO BLOCKS $DEV (device refused the image?)"; exit 1; }
  done
  echo "downloading $(date +%T)"
  START=$(date +%s); LASTP=""; LASTT=$START; OKT=0; LASTSHOW=""
  while true; do
    sleep 30
    R=$(log "$T" | grep -E "'$D'|$IEEE")
    NOW=$(date +%s)
    SHOW=$(echo "$R" | grep -iE "ota|update" | tail -1 | cut -c1-140)
    [ "$SHOW" != "${LASTSHOW:-}" ] && { echo "$SHOW"; LASTSHOW=$SHOW; }
    if echo "$R" | grep -qE "$FAIL"; then echo "FAILED $DEV"; echo "$R" | grep -E "$FAIL" | tail -2 | cut -c1-220; exit 1; fi
    echo "$R" | grep -qE "$OK_Z2M" && break
    if echo "$R" | grep -qE "$OK_ZH"; then
      # the transfer is done; Z2M logs 'Finished update' after the device announce (herdsman waits <= 120 s)
      [ "$OKT" -gt 0 ] || OKT=$NOW
      [ $(( NOW - OKT )) -ge 150 ] && { echo "transfer successful but no 'Finished update of '$DEV'' line (friendly name?): going on"; break; }
      continue
    fi
    P=$(echo "$R" | grep -E "OTA update of '$IEEE' (estimated|at)" | tail -1)
    [ -n "$P" ] && [ "$P" != "$LASTP" ] && { LASTP=$P; LASTT=$NOW; }   # a failed read is no progress
    [ $(( NOW - LASTT )) -ge "$STALL" ] && { echo "STALLED $DEV: no progress for $(( NOW - LASTT )) s"; exit 1; }
    [ $(( NOW - START )) -ge "$CAP" ] && { echo "TIMEOUT $DEV: still downloading after $(( NOW - START )) s"; exit 1; }
  done
  sleep 25
  T2=$(since)
  pub "zigbee2mqtt/$DEV/set" '{"read":{"cluster":"genBasic","attributes":["swBuildId",65298]}}'
  sleep 6
  echo "DONE $DEV: $(log "$T2" | grep -o "Read result of 'genBasic'.*" | tail -1 | cut -c1-200)"
done
echo "ALL DONE $(date +%T)"
