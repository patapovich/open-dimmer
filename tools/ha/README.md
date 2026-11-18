# Home Assistant / Zigbee2MQTT helpers

Settings come from the environment (all optional):

| Variable | Meaning | Default |
| --- | --- | --- |
| `HA_HELPERS` | directory of local helper scripts (OTA series and guard, below) | `~/ha` |
| `HA_ENV` | file with `HA_URL=` and `HA_TOKEN=` (a long-lived access token) | `$HA_HELPERS/.env` |
| `HA_SSH` | ssh target of the HA host whose `/config` holds `zigbee2mqtt` (SSH add-on) | `root@homeassistant.local` |
| `ZTU_DIR`, `ZTU_REMOTE` | checkout of the ZTU fork and its remote for patapovich/tuya-zigbee-switch | `~/projects/tzs-diag`, `patapovich` |

- `expose_dump.js <converter.js> [mcu_version]`: the ZDMS16-2 exposes the real
  zigbee-herdsman-converters computes from a generated converter (default MCU "2.1.9 own";
  pass "1.0.7" for a stock MCU). Run with `NODE_PATH=<zhc>/node_modules node expose_dump.js ...`
  against an unpacked zigbee-herdsman-converters 26.125.0 with its dependencies. Diff the sorted
  output of the deployed and the new converter before every converter deployment
  (docs/STATUS.md checklist).
- `ha_ws.py [--json] '<json command>' ...`: minimal Home Assistant websocket client (stdlib only,
  reads `$HA_ENV`), e.g. `{"type":"config/entity_registry/update","entity_id":"light.a","new_entity_id":"light.b"}`
  or `{"type":"switch_manager/config/save","config":{...}}`. `--json` prints each full result as
  one JSON line (e.g. `{"type":"config/device_registry/list"}`); the token is never printed.
- `configure_check.js <converter.js> [empty reads]`: runs the ZDMS16-2 configure of a generated
  converter with the real zhc against a fake device whose MCU answers late (`mcu_version` "" for
  the first reads) and prints the order and time of the reads.
