# Home Assistant / Zigbee2MQTT helpers

Settings come from the environment (all optional):

| Variable | Meaning | Default |
| --- | --- | --- |
| `HA_HELPERS` | directory of local helper scripts (OTA series and guard, below) | `~/ha` |
| `HA_ENV` | file with `HA_URL=` and `HA_TOKEN=` (a long-lived access token) | `$HA_HELPERS/.env` |
| `HA_SSH` | ssh target of the HA host whose `/config` holds `zigbee2mqtt` (SSH add-on) | `root@homeassistant.local` |
| `ZTU_DIR`, `ZTU_REMOTE` | checkout of the ZTU fork and its remote for patapovich/tuya-zigbee-switch | `~/projects/tzs-diag`, `patapovich` |

- `expose_dump.js <converter.js> [raw MCU version]`: the ZDMS16-2 exposes the real
  zigbee-herdsman-converters computes from a generated converter. The version is the raw Basic
  0xff12 value the Zigbee firmware reports, not what Zigbee2MQTT shows: default "2.1.9 own" (an
  MCU identified as Open Dimmer; shown as `mcu_version` 2.1.9, `mcu_firmware` Open Dimmer); pass
  "1.0.7" for a stock MCU or "" for one not known yet. Device-level exposes (`mcu_firmware`)
  print `endpoint=undefined`. Run with `NODE_PATH=<zhc>/node_modules node expose_dump.js ...`
  against an unpacked zigbee-herdsman-converters 26.125.0 with its dependencies. Diff the sorted
  output of the deployed and the new converter before every converter deployment
  (docs/STATUS.md checklist). The converter path may be relative.
- `ha_ws.py [--json] '<json command>' ...`: minimal Home Assistant websocket client (stdlib only,
  reads `$HA_ENV`), e.g. `{"type":"config/entity_registry/update","entity_id":"light.a","new_entity_id":"light.b"}`
  or `{"type":"switch_manager/config/save","config":{...}}`. `--json` prints each full result as
  one JSON line (e.g. `{"type":"config/device_registry/list"}`); the token is never printed.
- `configure_check.js <converter.js> [empty reads]`: runs the ZDMS16-2 configure of a generated
  converter with the real zhc against a fake device whose MCU answers late (raw version "" for
  the first reads, then "2.1.10 own", an Open Dimmer MCU) and prints the order and time of the
  reads.
- `deploy_ota.sh <ieee | friendly_name:ieee>...`: the local OTA step of a release. Takes the newest
  ZDMS16-2 image of the fork's `home-dimmer_build` branch, refuses it unless it is a valid OTA file
  that carries a valid PY32 image, shows each module's installed and the image's file version (from
  the HA update entity) and asks before a downgrade, then updates the modules one at a time with
  Z2M's `ota.image_block_response_delay` at 50 ms. A bare IEEE takes the Z2M friendly name from the
  HA device registry (the device `name`, not `name_by_user`), because Z2M logs the end and failures
  of an update with the friendly name. 250 ms is put back on every exit path (also on Ctrl-C), and a
  failed restore is reported loudly (exit 3). On Ctrl-C the series is stopped first and 250 ms is
  sent at once; a second Ctrl-C, TERM, HUP or a closed output pipe (`| tee` killed) cannot cut that
  cleanup short. It runs two local helpers from `$HA_HELPERS`, which read the Z2M log over
  `$HA_SSH` by full timestamp (the clocks must agree; checked first):
  `zdms16_ota_series.sh <image> <name:ieee>...` (offers the image to one module at a time through a
  local OTA index; exit 0 only when every module finished; stall and total timeouts) and
  `zdms16_ota_guard.sh <series output> <series pid>` (puts 250 ms back at the first BUFFER_FULL).
  The helpers must bound every ssh call (non-interactive, about 30 s), so that a dead connection
  is a failed read and the stall and total timeouts still fire.
