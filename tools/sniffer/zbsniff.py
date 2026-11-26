#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from the OQPSK PHY example of gr-ieee802-15-4: https://github.com/bastibl/gr-ieee802-15-4
"""zbsniff: IEEE 802.15.4 (Zigbee) channel sniffer with an ADALM-Pluto and GNU Radio.

One Zigbee channel, received with the Pluto at 4 MS/s (LO on the channel's centre; LO_OFFSET with
SAMP_RATE 8 MS/s would move it off, but a USB 2 bus drops samples above ~5 MS/s), shifted and
decimated to 4 MS/s when SAMP_RATE is higher, and demodulated with gr-ieee802-15-4's O-QPSK
receiver chain (quadrature demod, clock recovery, packet sink). An LO on the channel can disturb
a device right beside the Pluto (a battery remote lost downlink frames): keep the Pluto 1-2 m
away from the devices under test; the service powers both LOs down on stop. Every frame goes
into an hourly pcap (LINKTYPE_IEEE802_15_4_WITHFCS = 195): DIR/zb-YYYYmmdd-HH.pcap; finished
hours are gzipped, files older than KEEP_DAYS days are deleted (DIR/keep/ is never touched:
hard-link the files of an incident there to keep them). The captures are not decrypted here;
tshark does that at analysis time with a Wireshark key table (zbpcap.py).

The time stamps are the host's clock when the frame leaves the demodulator: a few ms late and
quantized to the Pluto buffer (BUFFER / SAMP_RATE = 2 ms); frames from one buffer share a
time. Order is exact, absolute times are not (use the devices' own counters for ms).

  zbsniff.py                 run (systemd zbsniff.service)
  zbsniff.py --gains 15,25,35,45 [--secs 60]   frames per gain, prints, no files
Config (env, or KEY=VALUE lines in $ZBSNIFF_CONF, default /etc/zbsniff/zbsniff.conf):
CHANNEL, GAIN, URI, DIR, KEEP_DAYS, SAMP_RATE, BUFFER, LO_OFFSET. Bad settings are refused before
the radio is touched, with exit status 78 (the unit does not restart on it).
"""
import gzip
import json
import os
import shutil
import struct
import sys
import threading
import time

DEFAULTS = {"CHANNEL": "11", "GAIN": "35", "URI": "usb:", "DIR": "/var/lib/zbsniff/pcap", "KEEP_DAYS": "7",
            "SAMP_RATE": "4000000", "BUFFER": "8192",    # 8192 samples at 4 MS/s = 2 ms: the time stamps' granularity
            "LO_OFFSET": "0"}   # an off-channel LO needs 8 MS/s: a USB 2 bus overflows above ~5 MS/s
CONF = os.environ.get("ZBSNIFF_CONF", "/etc/zbsniff/zbsniff.conf")
AGC = ("slow_attack", "fast_attack", "hybrid")
EX_CONFIG = 78    # bad settings; zbsniff.service: RestartPreventExitStatus=78 (a restart cannot fix them)


def load_conf():
    c = dict(DEFAULTS)
    try:
        with open(CONF) as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    c[k.strip()] = v.strip()
    except FileNotFoundError:
        pass
    for k in list(c):
        if k in os.environ:
            c[k] = os.environ[k]
    return c


def crc16_kermit(data):
    """IEEE 802.15.4 FCS (ITU-T CRC-16, LSB first, init 0)."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


def fcs_ok(frame):
    return len(frame) >= 3 and crc16_kermit(frame[:-2]) == struct.unpack_from("<H", frame, len(frame) - 2)[0]


class PcapWriter:
    """Hourly pcap files, gzip of finished hours, retention."""

    def __init__(self, d, keep_days):
        self.dir, self.keep_days = d, keep_days
        os.makedirs(os.path.join(d, "keep"), exist_ok=True)
        self.fh, self.name = None, None
        self.lock = threading.Lock()
        self.stats = {"frames": 0, "fcs_ok": 0, "fcs_bad": 0, "since": time.time(), "last_frame": None,
                      "minute": {"frames": 0, "fcs_ok": 0}, "last_minute": {"frames": 0, "fcs_ok": 0}}
        self._minute = int(time.time() // 60)

    @staticmethod
    def _good_end(path):
        """Length of the file's readable part: the pcap header and whole records (a torn tail
        from a killed process is cut off before appending)."""
        size = os.path.getsize(path)
        if size < 24:
            return 0
        with open(path, "rb") as fh:
            fh.seek(24)
            end = 24
            while True:
                h = fh.read(16)
                if len(h) < 16:
                    return end
                n = struct.unpack_from("<I", h, 8)[0]
                if n > 65535 or len(fh.read(n)) < n:
                    return end
                end += 16 + n

    def _open(self, t):
        name = os.path.join(self.dir, time.strftime("zb-%Y%m%d-%H.pcap", time.localtime(t)))
        if name == self.name:
            return
        old = self.name
        if self.fh:
            self.fh.close()
        good = self._good_end(name) if os.path.exists(name) else 0
        self.fh = open(name, "r+b" if os.path.exists(name) else "wb", buffering=0)
        self.fh.truncate(good)
        self.fh.seek(good)
        if good == 0:
            self.fh.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 195))
        self.name = name
        if old:
            threading.Thread(target=self._finish, args=(old,), daemon=True).start()

    def _finish(self, path):
        """gzip a finished hour, then retention."""
        try:
            with open(path, "rb") as src, gzip.open(path + ".gz.tmp", "wb", compresslevel=6) as dst:
                shutil.copyfileobj(src, dst)
            os.replace(path + ".gz.tmp", path + ".gz")
            st = os.stat(path)
            os.utime(path + ".gz", (st.st_atime, st.st_mtime))
            os.unlink(path)
        except OSError as e:
            print(f"gzip {path}: {e}", file=sys.stderr, flush=True)
            try:
                os.unlink(path + ".gz.tmp")
            except OSError:
                pass
        self.retention()

    def tidy(self):
        """At start: drop half-written .gz.tmp files and gzip hours an earlier run left open."""
        cur = os.path.join(self.dir, time.strftime("zb-%Y%m%d-%H.pcap", time.localtime()))
        for f in sorted(os.listdir(self.dir)):
            p = os.path.join(self.dir, f)
            if f.startswith("zb-") and f.endswith(".gz.tmp"):
                try:
                    os.unlink(p)
                except OSError:
                    pass
            elif f.startswith("zb-") and f.endswith(".pcap") and p != cur:
                if os.path.exists(p + ".gz"):
                    continue                    # a gzip finished but the unlink did not: keep both
                self._finish(p)

    def retention(self):
        limit = time.time() - self.keep_days * 86400
        for f in os.listdir(self.dir):
            p = os.path.join(self.dir, f)
            if f.startswith("zb-") and os.path.isfile(p) and p != self.name and os.stat(p).st_mtime < limit:
                try:
                    os.unlink(p)
                except OSError:
                    pass

    def write(self, frame, t=None):
        t = time.time() if t is None else t
        ok = fcs_ok(frame)
        with self.lock:
            self._open(t)
            sec = int(t)
            self.fh.write(struct.pack("<IIII", sec, int((t - sec) * 1e6), len(frame), len(frame)) + frame)
            s = self.stats
            s["frames"] += 1
            s["fcs_ok" if ok else "fcs_bad"] += 1
            s["last_frame"] = t
            m = int(t // 60)
            if m != self._minute:
                s["last_minute"], s["minute"], self._minute = s["minute"], {"frames": 0, "fcs_ok": 0}, m
            s["minute"]["frames"] += 1
            s["minute"]["fcs_ok"] += ok
        return ok

    def status(self, extra=None):
        with self.lock:
            m = int(time.time() // 60)
            if m != self._minute:              # no frames this minute: roll the counters anyway
                st = self.stats
                st["last_minute"] = st["minute"] if m == self._minute + 1 else {"frames": 0, "fcs_ok": 0}
                st["minute"], self._minute = {"frames": 0, "fcs_ok": 0}, m
            s = json.loads(json.dumps(self.stats))
        s["file"] = self.name
        s["t"] = time.time()
        if extra:
            s.update(extra)
        tmp = os.path.join(self.dir, "status.json.tmp")
        with open(tmp, "w") as fh:
            json.dump(s, fh)
        os.replace(tmp, os.path.join(self.dir, "status.json"))


def build(c, writer, gain):
    """The flowgraph: Pluto -> gr-ieee802-15-4's O-QPSK receiver (as in its OQPSK_PHY example)
    -> frames to writer.write()."""
    import pmt
    from gnuradio import analog, blocks, digital, gr, iio
    from gnuradio import filter as grfilter
    try:
        from gnuradio import ieee802_15_4
    except ImportError:                       # an OOT build installs it top-level
        import ieee802_15_4

    class FrameSink(gr.basic_block):
        def __init__(self, w):
            gr.basic_block.__init__(self, "zbsniff_frames", None, None)
            self.w = w
            self.message_port_register_in(pmt.intern("in"))
            self.set_msg_handler(pmt.intern("in"), self.handle)

        def handle(self, msg):
            t = time.time()
            body = pmt.cdr(msg) if pmt.is_pair(msg) else msg
            if not pmt.is_u8vector(body):           # make_blob() is a u8vector in PMT
                return
            frame = bytes(pmt.u8vector_elements(body))
            self.w.write(frame, t)

    tb = gr.top_block("zbsniff")
    rate = int(c["SAMP_RATE"])                 # checked by check_conf()
    decim = rate // 4000000                    # the receiver chain wants 4 MS/s (2 samples per chip)
    src = iio.fmcomms2_source_fc32(c["URI"], [True, True], int(c["BUFFER"]))
    src.set_len_tag_key("")
    src.set_frequency(lo_freq(c))
    src.set_samplerate(rate)
    if str(gain).lower() in AGC:
        src.set_gain_mode(0, str(gain).lower())
    else:
        src.set_gain_mode(0, "manual")
        src.set_gain(0, float(gain))
    src.set_quadrature(True)
    src.set_rfdc(True)
    src.set_bbdc(True)
    src.set_filter_params("Auto", "", 0, 0)
    taps = grfilter.firdes.low_pass(1.0, rate, 1.15e6, 0.7e6)
    xlate = grfilter.freq_xlating_fir_filter_ccf(decim, taps, -int(c["LO_OFFSET"]), rate)
    demod = analog.quadrature_demod_cf(1)
    dc = grfilter.single_pole_iir_filter_ff(0.00016)
    sub = blocks.sub_ff(1)
    mm = digital.clock_recovery_mm_ff(2, 0.000225, 0.5, 0.03, 0.0002)
    psink = ieee802_15_4.packet_sink(10)
    fsink = FrameSink(writer)
    tb.connect(src, xlate, demod)
    tb.connect(demod, (sub, 0))
    tb.connect(demod, dc, (sub, 1))
    tb.connect(sub, mm, psink)
    tb.msg_connect((psink, "out"), (fsink, "in"))
    tb.keep = (fsink,)
    return tb


def lo_freq(c):
    return int(2405e6 + 5e6 * (int(c["CHANNEL"]) - 11) + int(c["LO_OFFSET"]))


def tx_off(uri, rx_lo=None):
    """The Pluto boots with its TX path live (FDD, TX LO 2450 MHz, -10 dB). A sniffer never
    transmits: TX LO powered down, TX attenuation at maximum, DDS off; the RX LO is moved off
    the channel before it is powered up. Must run before the flowgraph holds the USB context
    (libiio allows one client); the settings last until the Pluto reboots."""
    import subprocess
    cmds = [["-o", "-c", "ad9361-phy", "voltage0", "hardwaregain", "-89.75"],
            ["-c", "cf-ad9361-dds-core-lpc", "altvoltage0", "raw", "0"],
            ["-c", "ad9361-phy", "altvoltage1", "powerdown", "1"]]
    if rx_lo:
        cmds += [["-c", "ad9361-phy", "altvoltage0", "frequency", str(rx_lo)],
                 ["-c", "ad9361-phy", "altvoltage0", "powerdown", "0"]]
    for a in cmds:
        subprocess.run(["iio_attr", "-u", uri] + a, capture_output=True, timeout=20)
    r = subprocess.run(["iio_attr", "-u", uri, "-c", "ad9361-phy", "altvoltage1", "powerdown"],
                       capture_output=True, text=True, timeout=20)
    if r.stdout.strip().splitlines()[-1:] != ["1"]:
        raise RuntimeError(f"Pluto TX LO not powered down: {r.stdout.strip()!r} {r.stderr.strip()!r}")


class Counter:
    """writer stand-in for the gain sweep."""

    def __init__(self):
        self.frames = self.ok = 0

    def write(self, frame, t=None):
        self.frames += 1
        self.ok += fcs_ok(frame)


def sweep(c, gains, secs):
    tx_off(c["URI"], lo_freq(c))
    for g in gains:
        cnt = Counter()
        tb = build(c, cnt, g)
        tb.start()
        time.sleep(secs)
        tb.stop()
        tb.wait()
        print(f"gain {g}: {cnt.frames} frames, {cnt.ok} FCS OK ({cnt.ok * 60 / secs:.0f}/min)", flush=True)


def check_conf(c, gains):
    """The settings' errors (empty list = all fine). Run before the radio is touched."""
    errs, num = [], {}
    for k in ("CHANNEL", "SAMP_RATE", "LO_OFFSET", "BUFFER", "KEEP_DAYS"):
        try:
            num[k] = int(c[k])
        except ValueError:
            errs.append(f"{k}={c[k]!r}: not a whole number")
    for g in gains:
        if g.lower() not in AGC:
            try:
                float(g)
            except ValueError:
                errs.append(f"gain {g!r}: dB, or one of {', '.join(AGC)}")
    if errs:
        return errs
    rate, off = num["SAMP_RATE"], num["LO_OFFSET"]
    if not 11 <= num["CHANNEL"] <= 26:
        errs.append(f"CHANNEL={num['CHANNEL']}: 11-26")
    if rate < 4000000 or rate % 4000000:
        errs.append(f"SAMP_RATE={rate}: must be a multiple of 4000000 (the receiver runs at 4 MS/s)")
    elif off and (rate < 8000000 or abs(off) > rate // 2 - 1150000):
        errs.append(f"LO_OFFSET={off}: needs SAMP_RATE=8000000 or more and |LO_OFFSET| <= SAMP_RATE / 2 - 1150000 "
                    "(the channel must stay inside the received band)")
    if num["BUFFER"] <= 0:
        errs.append(f"BUFFER={num['BUFFER']}: must be positive")
    if num["KEEP_DAYS"] < 0:
        errs.append(f"KEEP_DAYS={num['KEEP_DAYS']}: must not be negative")
    return errs


def main():
    c = load_conf()
    args = sys.argv[1:]
    gains, secs = [c["GAIN"]], 60
    try:
        if "--gains" in args:
            gains = [g.strip() for g in args[args.index("--gains") + 1].split(",")]
        if "--secs" in args:
            secs = int(args[args.index("--secs") + 1])
            if secs <= 0:
                raise ValueError(secs)
    except (IndexError, ValueError):
        print("usage: zbsniff.py [--gains G1,G2,... [--secs N]]", file=sys.stderr)
        sys.exit(2)
    errs = check_conf(c, gains)
    if errs:
        for e in errs:
            print(f"zbsniff: bad setting: {e} (settings: {CONF} or the environment)", file=sys.stderr)
        sys.exit(EX_CONFIG)
    if "--gains" in args:
        sweep(c, gains, secs)
        return
    tx_off(c["URI"], lo_freq(c))              # raises: systemd restarts, never runs with TX live
    w = PcapWriter(c["DIR"], int(c["KEEP_DAYS"]))
    w.tidy()
    w.retention()
    tb = build(c, w, c["GAIN"])
    tb.start()
    print(f"zbsniff: channel {c['CHANNEL']}, LO {lo_freq(c) / 1e6:.3f} MHz, {c['SAMP_RATE']} S/s, gain {c['GAIN']}, "
          f"{c['DIR']}", flush=True)
    started = time.time()
    while True:
        time.sleep(60)
        w.status({"channel": int(c["CHANNEL"]), "gain": c["GAIN"]})
        last = w.stats["last_frame"] or started
        if time.time() - last > 600:          # a busy channel is never quiet for 10 min: the radio is gone
            print("zbsniff: no frames for 10 min, exiting for a restart", file=sys.stderr, flush=True)
            os._exit(1)


if __name__ == "__main__":
    main()
