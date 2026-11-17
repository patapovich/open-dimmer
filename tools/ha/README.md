# Home Assistant / Zigbee2MQTT helpers

- `expose_dump.js <converter.js> [mcu_version]`: the ZDMS16-2 exposes the real
  zigbee-herdsman-converters computes from a generated converter (default MCU "2.1.9 own";
  pass "1.0.7" for a stock MCU). Run with `NODE_PATH=<zhc>/node_modules node expose_dump.js ...`
  from an unpacked zigbee-herdsman-converters 26.125.0 with its dependencies (the session used
  `zhc-26.125.0/` in its scratchpad). Diff the sorted output of the deployed and the new
  converter before every converter deployment (docs/STATUS.md checklist).
- `ha_ws.py '<json command>' ...`: minimal Home Assistant websocket client (stdlib only, reads
  `~/ha-claude/.env`), e.g. `{"type":"config/entity_registry/update","entity_id":"light.a","new_entity_id":"light.b"}`
  or `{"type":"switch_manager/config/save","config":{...}}`.
- `configure_check.js <converter.js> [empty reads]`: runs the ZDMS16-2 configure of a generated
  converter with the real zhc against a fake device whose MCU answers late (`mcu_version` "" for
  the first reads) and prints the order and time of the reads.
