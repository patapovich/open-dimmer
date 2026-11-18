const converter = require("path").resolve(process.argv[2]);   // a relative path works too
const zhc = require("zigbee-herdsman-converters");
const defs = require(converter);
const list = Array.isArray(defs) ? defs : (defs.default || defs.definitions || [defs]);
const def = list.find((d) => d.model === "ZDMS16-2");
const prepared = zhc.prepareDefinition ? zhc.prepareDefinition(def) : def;
const V = process.argv[3] || "2.1.9 own";
const dev = { ieeeAddr: "0x0000000000000001", meta: { mcuVersion: V }, isDummyDevice: false,
  getEndpoint: () => ({ clusters: { genBasic: { attributes: { 65298: V } } } }), endpoints: [] };
const ex = typeof prepared.exposes === "function" ? prepared.exposes(dev, {}) : prepared.exposes;
for (const x of ex) console.log(`${x.type.padEnd(8)} ${String(x.name || x.property).padEnd(22)} endpoint=${x.endpoint}`);
