// Run the ZDMS16-2 configure of a generated converter with the real zigbee-herdsman-converters
// against a fake device whose MCU answers late (mcu_version "" at first), and print the order of
// the reads. Usage: NODE_PATH=<zhc>/node_modules node configure_check.js <converter.js> [empty reads]
const path = process.argv[2];
const emptyReads = Number(process.argv[3] ?? 2);
const zhc = require("zigbee-herdsman-converters");
const defs = require(path);
const list = Array.isArray(defs) ? defs : (defs.default || defs.definitions || [defs]);
const def = zhc.prepareDefinition(list.find((d) => d.model === "ZDMS16-2"));
const t0 = Date.now();
const log = [];
let versionReads = 0;
class Endpoint {
    constructor(id) {
        Object.assign(this, {
    ID: id, deviceIeeeAddress: "0x0000000000000001",
    read: async (cluster, attrs) => {
        const ms = Date.now() - t0;
        if (cluster === "genBasic" && attrs.includes(0xff12)) {
            versionReads++;
            const v = versionReads > emptyReads ? "2.1.10 own" : "";
            log.push(`${ms}ms ep${id} read mcu_version -> "${v}"`);
            return {[0xff12]: v};
        }
        log.push(`${ms}ms ep${id} read ${cluster} ${attrs.map((a) => (typeof a === "number" ? "0x" + a.toString(16) : a)).join(",")}`);
        return {};
    },
    bind: async () => {},
    unbind: async () => {},
    configureReporting: async () => {},
    write: async () => {},
    supportsInputCluster: () => true,
    supportsOutputCluster: () => true,
    getClusterAttributeValue: () => undefined,
    saveClusterAttributeKeyValue: () => {},
    clusters: {},
    inputClusters: [],
    outputClusters: [],
});
    }
}
const endpoint = (id) => new Endpoint(id);
const eps = {1: endpoint(1), 2: endpoint(2)};
class Device {}
const device = Object.assign(new Device(), { ieeeAddr: "0x0000000000000001", meta: {}, getEndpoint: (id) => eps[id], endpoints: [eps[1], eps[2]], save: () => {} });
(async () => {
    await def.configure(device, eps[1], def);
    for (const l of log) console.log(l);
})().catch((e) => { console.error("configure failed:", e); process.exit(1); });
