#!/bin/bash
# While the OTA series runs: put Z2M's ota.image_block_response_delay back to 250 ms (Z2M's
# default) at the first BUFFER_FULL in the Z2M log, or when the series has ended (done or stopped).
# deploy_ota.sh starts it beside zdms16_ota_series.sh. BUFFER_FULL is the Z-Stack coordinator's
# status 0x11 (no room for another send): the coordinator is overloaded, so the OTA slows down
# at once instead of adding to the load. The log lines matched: 'BUFFER_FULL' or '0x11 '. With
# another adapter they may never appear; 250 ms is then put back when the series ends.
# Usage: zdms16_ota_guard.sh <output file of zdms16_ota_series.sh> [<series pid>]
#   With the pid the guard follows that process; without it, any running zdms16_ota_series.sh.
# Checks every 20 s. Exit 0 when HA accepted the restore, 1 when it did not (then set 250 in Z2M
# by hand: Settings, OTA updates); 2 for a usage error (no output file given) or when HA_URL or
# HA_TOKEN cannot be read.
# Env: HA_ENV (file with HA_URL= and HA_TOKEN=; default $HA_HELPERS/.env, or ~/ha/.env when
#        HA_HELPERS is not set),
#      HA_SSH (ssh target of the HA host whose /config holds zigbee2mqtt, default
#        root@homeassistant.local).
# A log read ends within 30 s (a dead connection = not checked), so the end check always runs.
set -u
[ $# -ge 1 ] && [ -n "$1" ] ||
  { echo "usage: zdms16_ota_guard.sh <output file of zdms16_ota_series.sh> [<series pid>]"; exit 2; }
HA_ENV=${HA_ENV:-${HA_HELPERS:-$HOME/ha}/.env}
# shellcheck disable=SC1090
source "$HA_ENV" && [ -n "${HA_URL:-}" ] && [ -n "${HA_TOKEN:-}" ] ||
  { echo "!!! cannot read HA_URL and HA_TOKEN from $HA_ENV (set HA_ENV): the guard cannot put 250 ms back"; exit 2; }
HA_SSH=${HA_SSH:-root@homeassistant.local}
SSH=(timeout --foreground -k 5 30 ssh -n -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 "$HA_SSH")
OUT=$1
SP=${2:-}
T0=$(date -d "@$(( $(date +%s) - 10 ))" '+%F %T')   # once, full timestamp: midnight-safe
ENDS='ALL DONE|FAILED|NO START|NO BLOCKS|STALLED|TIMEOUT|STOPPED'
restore() {
  if curl -fsS --retry 3 --retry-connrefused --retry-delay 2 --max-time 20 -o /dev/null -X POST \
      -H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json" "$HA_URL/api/services/mqtt/publish" \
      -d '{"topic":"zigbee2mqtt/bridge/request/options","payload":"{\"options\":{\"ota\":{\"image_block_response_delay\":250}}}"}'; then
    echo "delay 250 sent at $(date +%T) (HA accepted the publish): $1"; exit 0
  fi
  echo "!!! FAILED to restore image_block_response_delay 250 at $(date +%T) ($1): set it in Z2M by hand"; exit 1
}
running() { if [ -n "$SP" ]; then kill -0 "$SP" 2>/dev/null; else pgrep -f zdms16_ota_series.sh >/dev/null; fi; }
while true; do
  sleep 20
  # shellcheck disable=SC2029
  if B=$("${SSH[@]}" "L=\$(ls -1 /config/zigbee2mqtt/log | tail -1); awk -v t='[$T0' 'substr(\$0, 1, 1) == \"[\" { ok = substr(\$0, 1, length(t)) >= t } ok' /config/zigbee2mqtt/log/\$L/log.log | grep -c -E 'BUFFER_FULL|0x11 ' || true"); then
    if [ "${B:-0}" -gt 0 ]; then restore "BUFFER_FULL seen ($B lines)"; fi
  else
    echo "reading the Z2M log failed at $(date +%T) (BUFFER_FULL not checked)"
  fi
  if grep -q -E "$ENDS" "$OUT" || ! running; then restore "series ended: $(grep -E "$ENDS" "$OUT" | tail -1)"; fi
done
