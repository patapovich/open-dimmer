# Home Assistant / Zigbee2MQTT helpers

Settings come from the environment (all optional):

| Variable | Meaning | Default |
| --- | --- | --- |
| `HA_HELPERS` | directory of the OTA helpers `zdms16_ota_series.sh` and `zdms16_ota_guard.sh` (below) | this directory |
| `HA_ENV` | file with `HA_URL=` and `HA_TOKEN=` (a long-lived access token) | `$HA_HELPERS/.env` when `HA_HELPERS` is set, else `~/ha/.env` |
| `HA_SSH` | ssh target of the HA host whose `/config` holds `zigbee2mqtt` (SSH add-on) | `root@homeassistant.local` |
| `ZTU_DIR`, `ZTU_REMOTE` | checkout of the ZTU fork and its remote for patapovich/tuya-zigbee-switch | `~/projects/tzs-diag`, `patapovich` |

The `HA_ENV` file holds a token. Keep it outside this repository: its default never points here.

- `expose_dump.js <converter.js> [raw MCU version]`: the ZDMS16-2 exposes the real
  zigbee-herdsman-converters computes from a generated converter. The version is the raw Basic
  0xff12 value the Zigbee firmware reports, not what Zigbee2MQTT shows: default "2.1.9 own" (an
  MCU identified as Open Dimmer; shown as `mcu_version` 2.1.9, `mcu_firmware` Open Dimmer); pass
  "1.0.7" for a stock MCU or "" for one not known yet (as right after a restart: the Open Dimmer
  settings are hidden, the same set as with "1.0.7"). Device-level exposes (`mcu_firmware`)
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
  cleanup short. `IMAGE=<file.zigbee>` deploys a local build instead; `STOCK=1` converts modules
  that still run the Tuya stock Zigbee firmware (see the script header). It runs the two OTA
  helpers below from `$HA_HELPERS`. They read the Z2M log over `$HA_SSH` by full timestamp, so the
  clocks here and on the HA host must agree (checked first). A replacement helper must bound every
  ssh call (non-interactive, about 30 s), so that a dead connection is a failed read and the stall
  and total timeouts still fire.
- `zdms16_ota_series.sh <image> <friendly_name:ieee>...`: updates the modules one at a time.
  `<image>` is a file name in `/config/zigbee2mqtt/images` on the HA host (`deploy_ota.sh` copies
  it there). For each module it writes a local OTA index with one entry for the image
  (`force: true`) and asks Z2M to update the module. It clears the index (`[]`) as soon as the
  update has started: the image is offered no longer than that. It then follows the Z2M log until
  `Finished update of '<friendly name>'`, and reads `swBuildId` and the raw MCU version (Basic
  0xff12) back. It stops at the first module that does not finish: `FAILED`, `NO START` (no start
  within 90 s), `NO BLOCKS` (no block request within 75 s: the module refused the image),
  `STALLED` (no progress for `OTA_STALL_S`), `TIMEOUT` (download longer than `OTA_CAP_S`) or
  `STOPPED` (a signal). Exit 0 after `ALL DONE`, 1 at a stop, 130 at `STOPPED` by Ctrl-C (INT),
  143 at `STOPPED` by TERM or HUP, 2 for bad arguments or settings. The index is cleared on every
  exit.
- `zdms16_ota_guard.sh <series output> [<series pid>]`: runs beside the series. Every 20 s it
  counts the `BUFFER_FULL` and `0x11 ` lines in the Z2M log since its start. At the first one, or
  when the series has ended (an end line in its output, or the process is gone), it sets
  `image_block_response_delay` back to 250 ms and exits: 0 when HA accepted the publish, 1 when it
  did not (set 250 in Z2M by hand), 2 for a usage error (no output file given) or when it cannot
  read `HA_URL` and `HA_TOKEN`. Without the pid it follows any running `zdms16_ota_series.sh`.

Run the two helpers by hand only for a test. `deploy_ota.sh` also checks the image and the
clocks, copies the image, and sets and restores the delay.

## OTA block delay and BUFFER_FULL

Z2M waits `ota.image_block_response_delay` before it answers each image block request. 250 ms
is Z2M's default: about 17 min per module. 50 ms is Z2M's minimum and much faster, but it loads
the coordinator more. The setting is Z2M-wide: other OTA updates get it too. So `deploy_ota.sh`
sets `OTA_DELAY_MS` (default 50, 50..250) only while the series runs, with an MQTT publish to
`zigbee2mqtt/bridge/request/options` through HA. 250 ms is put back:

- by the guard, at the first BUFFER_FULL in the Z2M log;
- by the guard, when the series ends;
- by `deploy_ota.sh`, on every exit path (also on Ctrl-C, TERM and HUP).

BUFFER_FULL is the Z-Stack coordinator's status 0x11: it has no room for another send. The OTA
then slows down at once instead of adding to the load. With another adapter these lines may
never appear; 250 ms is then put back when the series ends. When HA does not accept a restore,
the scripts say so loudly (`deploy_ota.sh` exits 3): set 250 in Z2M by hand (Settings, OTA
updates).

## What the HA host needs

- SSH with a key (the scripts use `BatchMode`), for example the Advanced SSH & Web Terminal
  add-on, on the host whose `/config/zigbee2mqtt` is Z2M's data directory (the Zigbee2MQTT
  add-on's default).
- Z2M's OTA index override file name (Settings, OTA updates) set to `local_ota_index.json` in that
  directory, then a Z2M restart (once). The series owns this file: it leaves `[]` in it, and any
  other entry is lost.
- The directory `/config/zigbee2mqtt/images`.
- Z2M's file log (`/config/zigbee2mqtt/log/<newest>/log.log`) with the default timestamp format
  `YYYY-MM-DD HH:mm:ss`. The series follows the lines Zigbee2MQTT and zigbee-herdsman log during an
  update (`OTA update of '<ieee>' at ...%`, `Finished update of '<friendly name>'`). If the log
  level hides them, the series stops with `NO START`.
- The HA MQTT integration on Z2M's broker: the scripts publish through the `mqtt.publish` service.

## OTA settings

All optional:

| Variable | Script | Meaning | Default |
| --- | --- | --- | --- |
| `OTA_DELAY_MS` | `deploy_ota.sh` | Z2M's block response delay while the series runs, 50..250 ms | `50` |
| `IMAGE` | `deploy_ota.sh` | a local OTA file (.zigbee) instead of the newest `home-dimmer_build` image | unset |
| `STOCK` | `deploy_ota.sh` | `1` converts modules that run the Tuya stock Zigbee firmware | `0` |
| `HA_WS` | `deploy_ota.sh` | websocket client for the registry lookup | `ha_ws.py` here |
| `OTA_STALL_S` | series | no new progress line for this long (s) = `STALLED` | `300` |
| `OTA_CAP_S` | series | longest download of one module (s) = `TIMEOUT` | `2700` |
| `OTA_INDEX_MODEL` | series | `modelId` of the index entry | `TS0601-ZDMS16-2` |
| `OTA_INDEX_MANUF` | series | `manufacturerName` list of the index entry, comma-separated | `jtbgusdc` |
| `OTA_INDEX_FORCE` | series | `1` = `force: true` in the index entry | `1` |

With `STOCK=1`, `deploy_ota.sh` sets the index entry itself: `TS0601`, the modules' stock
manufacturer names (from the Z2M database) and no force.

## Credits

- [Zigbee2MQTT](https://www.zigbee2mqtt.io/) and
  [zigbee-herdsman](https://github.com/Koenkk/zigbee-herdsman): the OTA update, the local OTA
  index, the bridge requests and the log lines these scripts follow.
- [Home Assistant](https://www.home-assistant.io/): the `mqtt.publish` service and the websocket
  API.
- [romasku/tuya-zigbee-switch](https://github.com/romasku/tuya-zigbee-switch): the local OTA index
  and faster OTA settings
  ([docs/updating.md](https://github.com/romasku/tuya-zigbee-switch/blob/main/docs/updating.md)).
