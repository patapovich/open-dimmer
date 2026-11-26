#!/usr/bin/env python3
"""zbpcap: what the air says about one node, from zbsniff's pcaps.

MAC level, from tshark fields: every frame to or from the node, the ACKs paired to them (an ACK
has no addresses: it belongs to the frame right before it in capture order when that frame
asked for one and has the same MAC sequence number), the frame pending bit of the ACK to each
poll (Data Request), and per FP=1 poll whether the parent's frame came before the node's next
poll. Frames with a bad FCS are counted apart and never paired or trusted.
Timing: zbsniff's time stamps are quantized to its Pluto buffer (2 ms) and a few ms late; the
order is exact. For ms-level poll->TX delays use the devices' own counters.
With the key table zigbee_pc_keys in $WIRESHARK_CONFIG_DIR (default /etc/zbsniff/wireshark,
set_key.sh) tshark decrypts the NWK layer: NWK command ids and APS cluster / ZCL command are
shown. Only the fields below are asked from tshark: never key material (no verbose decode of
APS Transport Key frames).

  zbpcap.py --node NWK [--parent NWK] [--minutes N | --since 'YYYY-mm-dd HH:MM'] [--list] [--dir DIR] [file ...]
--parent: default the address the node's polls go to most often.
Without files: the hour files of DIR (default /var/lib/zbsniff/pcap) covering the window
(default the last 60 min). With files and no --minutes/--since: the whole files.
"""
import collections
import glob
import os
import subprocess
import sys
import time

FIELDS = ["frame.time_epoch", "wpan.frame_type", "wpan.seq_no", "wpan.src16", "wpan.dst16", "wpan.pending",
          "wpan.ack_request", "wpan.cmd", "wpan.fcs_ok", "frame.len", "zbee_nwk.src", "zbee_nwk.dst",
          "zbee_nwk.cmd.id", "zbee_aps.cluster", "zbee_zcl.cmd.id"]
TYPES = {0: "beacon", 1: "data", 2: "ack", 3: "cmd"}
CMDS = {1: "assoc-req", 2: "assoc-rsp", 3: "disassoc", 4: "data-req", 7: "beacon-req", 8: "coord-realign"}
KEYDIR = os.environ.get("WIRESHARK_CONFIG_DIR", "/etc/zbsniff/wireshark")
OPTS_WITH_VALUE = ("--node", "--parent", "--minutes", "--since", "--dir")


def arg(name, default=None):
    a = sys.argv
    return a[a.index(name) + 1] if name in a else default


def positional():
    out, skip = [], False
    for a in sys.argv[1:]:
        if skip:
            skip = False
        elif a in OPTS_WITH_VALUE:
            skip = True
        elif not a.startswith("--"):
            out.append(a)
    return out


def h16(v):
    return int(v, 16) if v.lower().startswith("0x") else int(v)


def true(v):
    return v in ("1", "True", "true")


def hour_files(d, since):
    """zb-YYYYmmdd-HH.pcap[.gz] whose hour overlaps [since, now] (no .tmp)."""
    out = []
    for p in sorted(glob.glob(os.path.join(d, "zb-*.pcap")) + glob.glob(os.path.join(d, "zb-*.pcap.gz"))):
        stem = os.path.basename(p).split(".")[0]          # zb-YYYYmmdd-HH
        try:
            start = time.mktime(time.strptime(stem[3:], "%Y%m%d-%H"))
        except ValueError:
            continue
        if start + 3600 > since:
            out.append((start, p))
    out.sort()
    if out and out[0][0] > since + 60:
        print(f"note: captures start {time.strftime('%Y-%m-%d %H:%M', time.localtime(out[0][0]))}, "
              f"after the window's start", file=sys.stderr)
    return [p for _, p in out]


def frames(files):
    cmd = ["tshark", "-n", "-T", "fields", "-E", "separator=\t", "-E", "occurrence=f"]
    for f in FIELDS:
        cmd += ["-e", f]
    env = dict(os.environ, WIRESHARK_CONFIG_DIR=KEYDIR)   # the key table, whoever runs this
    out = []
    for path in files:
        r = subprocess.run(cmd + ["-r", path], capture_output=True, text=True, env=env)
        if r.returncode != 0:
            print(f"tshark {path}: exit {r.returncode}: {r.stderr.strip()[-300:]}", file=sys.stderr)
        for line in r.stdout.splitlines():
            v = dict(zip(FIELDS, line.split("\t")))
            try:
                v["t"] = float(v["frame.time_epoch"])
            except (ValueError, KeyError):
                continue
            out.append(v)
    out.sort(key=lambda x: x["t"])                    # stable: capture order kept within a time stamp
    return out


def main():
    if "-h" in sys.argv or "--help" in sys.argv:
        print(__doc__)
        return
    if not arg("--node"):
        sys.exit("--node NWK is required (the node's short address, e.g. --node 0x1234)")
    node = h16(arg("--node"))
    parent = h16(arg("--parent")) if arg("--parent") else None
    d = arg("--dir", "/var/lib/zbsniff/pcap")
    files = positional()
    missing = [f for f in files if not os.path.isfile(f)]
    if missing:
        sys.exit(f"no such file: {', '.join(missing)}")
    if arg("--since"):
        since = time.mktime(time.strptime(arg("--since"), "%Y-%m-%d %H:%M"))
    elif arg("--minutes") or not files:
        since = time.time() - float(arg("--minutes", "60")) * 60
    else:
        since = 0.0
    if not files:
        files = hour_files(d, since)
    fr = [f for f in frames(files) if f["t"] >= since]
    rows, polls, bad = [], [], 0
    prev = None                                     # the frame before, in capture order
    decrypted = 0
    for f in fr:
        typ = int(f["wpan.frame_type"], 0) if f["wpan.frame_type"] else -1
        seq = int(f["wpan.seq_no"]) if f["wpan.seq_no"] else None
        ok = true(f["wpan.fcs_ok"]) or f["wpan.fcs_ok"] == ""
        if f["zbee_aps.cluster"] or f["zbee_nwk.cmd.id"]:
            decrypted += 1
        if typ == 2:
            if ok and prev is not None and prev.get("seq") == seq and true(prev["wpan.ack_request"]) \
                    and f["t"] - prev["t"] < 0.020:
                prev["acked"] = f
            prev = None                             # one ACK per frame
            continue
        src = h16(f["wpan.src16"]) if f["wpan.src16"] else None
        dst = h16(f["wpan.dst16"]) if f["wpan.dst16"] else None
        f["typ"], f["src"], f["dst"], f["seq"], f["ok"] = typ, src, dst, seq, ok
        prev = f if ok else None
        if node in (src, dst):
            if not ok:
                bad += 1
                continue
            rows.append(f)
            if typ == 3 and f["wpan.cmd"] and int(f["wpan.cmd"], 0) == 4 and src == node:
                polls.append(f)
    if parent is None:                              # a poll (Data Request) goes to the node's parent
        seen = collections.Counter(p["dst"] for p in polls if p["dst"] is not None)
        parent = seen.most_common(1)[0][0] if seen else 0x0000
    # per poll: the ACK's frame pending bit; after an FP=1 poll the parent's frame before the next poll
    for i, p in enumerate(polls):
        a = p.get("acked")
        p["fp"] = true(a["wpan.pending"]) if a else None
        nxt_poll = polls[i + 1]["t"] if i + 1 < len(polls) else float("inf")
        nxt = next((r for r in rows if p["t"] < r["t"] < nxt_poll and r["src"] == parent and r["dst"] == node), None)
        p["delay_ms"] = round((nxt["t"] - p["t"]) * 1000, 1) if nxt and nxt["t"] - p["t"] < 1 else None
    to_node = [r for r in rows if r["dst"] == node and r["typ"] in (1, 3)]
    from_node = [r for r in rows if r["src"] == node and r["typ"] in (1, 3)]
    from_ar = [r for r in from_node if true(r["wpan.ack_request"])]     # broadcasts ask for no ACK
    retries, last = 0, {}
    for r in to_node:                               # same (src, seq) again within 100 ms = MAC retry
        k = (r["src"], r["seq"])
        if k in last and r["t"] - last[k] < 0.1:
            retries += 1
        last[k] = r["t"]
    print(f"{len(fr)} frames in {len(files)} file(s) since {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(since))}"
          f" ({decrypted} decrypted); node 0x{node:04x}: {len(from_node)} frames from it ({len(polls)} polls), "
          f"{len(to_node)} to it, {bad} with a bad FCS (left out); time stamps +-2 ms")
    if polls:
        fp1 = [p for p in polls if p["fp"]]
        print(f"polls: ACK heard {sum(1 for p in polls if p.get('acked'))}/{len(polls)}, FP=1 {len(fp1)}, "
              f"parent 0x{parent:04x} frame before the next poll: {sum(1 for p in fp1 if p['delay_ms'] is not None)}/{len(fp1)}, "
              f"delays (ms, +-2) {[p['delay_ms'] for p in fp1][:20]}")
    if to_node:
        print(f"frames to node, its ACK heard: {sum(1 for r in to_node if r.get('acked'))}/{len(to_node)} "
              f"(MAC retries: {retries})")
    if from_node:
        print(f"frames from node asking for an ACK, ACK heard: {sum(1 for r in from_ar if r.get('acked'))}/{len(from_ar)}")
    if "--list" in sys.argv:
        for r in rows:
            kind = TYPES.get(r["typ"], f"type{r['typ']}")
            if r["typ"] == 3 and r["wpan.cmd"]:
                kind = CMDS.get(int(r["wpan.cmd"], 0), f"cmd{r['wpan.cmd']}")
            a = r.get("acked")
            ack = ("ACK" + (" FP" if true(a["wpan.pending"]) else "")) if a else ("noACK" if true(r["wpan.ack_request"]) else "")
            extra = " ".join(x for x in (
                f"nwk {r['zbee_nwk.src']}->{r['zbee_nwk.dst']}" if r["zbee_nwk.src"] else "",
                f"nwkcmd {r['zbee_nwk.cmd.id']}" if r["zbee_nwk.cmd.id"] else "",
                f"cl {r['zbee_aps.cluster']}" if r["zbee_aps.cluster"] else "",
                f"zcl {r['zbee_zcl.cmd.id']}" if r["zbee_zcl.cmd.id"] else "") if x)
            t = r["t"]
            src = f"0x{r['src']:04x}" if r["src"] is not None else "-"
            dst = f"0x{r['dst']:04x}" if r["dst"] is not None else "-"
            print(f"{time.strftime('%H:%M:%S', time.localtime(t))}.{int(t % 1 * 1000):03d} {kind:9s} {src}->{dst} "
                  f"seq {r['seq']} {ack:7s} {r['frame.len']}B {extra}")


if __name__ == "__main__":
    main()
