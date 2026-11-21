#!/usr/bin/env python3
"""custom_curve.py: measure the lamp on an Open Dimmer channel and give it a custom dimming curve
with even perceived steps (Open Dimmer 2.2.8 or later, through Zigbee2MQTT).

The built-in curves are made for LED lamps in general; a given lamp (its driver) spreads its
light in its own way. This tool measures the light of the lamp at a number of levels, each
switched on from dark (with the linear curve, so the conduction of every level is known), fits
the lamp's light against the conduction and computes the 17 points of a curve whose perceived
lightness (CIE L*) rises evenly from the minimum to the maximum. It then writes the curve
(`custom_curve_<channel>`) and selects it (`dimming_curve_<channel>: custom`).

The light is read from an illuminance sensor that Zigbee2MQTT knows (put it next to the lamp,
facing it, out of daylight; nobody moving in front of it), or typed in from a light meter
(a phone app) with --manual. Run it in the dark: daylight and other lamps change the reading.

The curve is made for the channel's min_level / max_level at the time: measure again after
changing them. The lamp compensation (lamp_compensation) does not change the measurement (every
level is switched on from dark).

Connection, one of:
  --ha URL         Home Assistant (its MQTT integration publishes to Zigbee2MQTT), with a long-lived
                   access token in the environment variable HA_TOKEN
  --mqtt HOST      the MQTT broker of Zigbee2MQTT ([--port 1883] [--username U], password in the
                   environment variable MQTT_PASSWORD); needs paho-mqtt (pip install paho-mqtt)

Examples:
  HA_TOKEN=... python3 tools/custom_curve.py --ha http://homeassistant.local:8123 \\
      --device bedroom-dimmer --channel l1 --sensor bedroom-presence --apply
  python3 tools/custom_curve.py --mqtt mqtt.local --username z2m --device bedroom-dimmer \\
      --manual --save lamp.json
  python3 tools/custom_curve.py --load lamp.json            # the curve again, from a saved run
  python3 tools/custom_curve.py --self-test

A sensor whose reading is not proportional to lux (many cheap ones are not) can be mapped with
--calibration FILE: a JSON list of [reading, lux] pairs taken with a light meter at the same
place (dark, the minimum, a few levels, full). The tool interpolates ln(lux) between them.
"""

import argparse
import base64
import json
import math
import os
import select
import socket
import struct
import sys
import time
from urllib.parse import urlparse

POINTS = 17                    # the custom curve: outputs at 0, 1/16 .. 1 of min..max
OUT_MAX = 10000


# ---- the maths (pure: --self-test, --load) ---------------------------------------------------

def lstar(y, ymax):
    """CIE L* (0..100) of luminance y against the full ymax."""
    t = max(y, 0.0) / ymax
    return 116.0 * t ** (1 / 3) - 16.0 if t > 0.008856 else 903.3 * t


def lstar_inverse(L, ymax):
    t = ((L + 16.0) / 116.0) ** 3 if L > 8.0 else L / 903.3
    return t * ymax


def monotone(points):
    """Non-decreasing least-squares fit (pool adjacent violators) of (x, y) points, sorted by x:
    [(x, y)] with one point per block (x the block's mean)."""
    blocks = []
    for x, y in sorted(points):
        blocks.append([x, y, 1])
        while len(blocks) > 1 and blocks[-2][1] / blocks[-2][2] > blocks[-1][1] / blocks[-1][2]:
            x2, y2, n2 = blocks.pop()
            blocks[-1][0] += x2
            blocks[-1][1] += y2
            blocks[-1][2] += n2
    return [(x / n, y / n) for x, y, n in blocks]


def interp(fit, x):
    """Piecewise-linear value of a monotone fit at x (held at the ends)."""
    if x <= fit[0][0]:
        return fit[0][1]
    for (x1, y1), (x2, y2) in zip(fit, fit[1:]):
        if x <= x2:
            return y1 + (y2 - y1) * (x - x1) / (x2 - x1) if x2 > x1 else y2
    return fit[-1][1]


def first_reaching(fit, y):
    """The smallest x where the monotone fit reaches y (its first x below, its last above)."""
    if y <= fit[0][1]:
        return fit[0][0]
    for (x1, y1), (x2, y2) in zip(fit, fit[1:]):
        if y <= y2:
            return x1 + (x2 - x1) * (y - y1) / (y2 - y1) if y2 > y1 else x1
    return fit[-1][0]


def make_curve(samples):
    """samples: [(f, light)], f = the fraction of the channel's min..max (0 = min, 1 = max) with
    the linear curve, light net of the ambient. Returns the 17 points (0..10000) for an even
    perceived lightness from f = 0 to f = 1, and the fit."""
    fit = monotone([(f, max(y, 0.0)) for f, y in samples])
    ymax = interp(fit, 1.0)
    if ymax <= 0:
        raise ValueError("no light measured at the maximum")
    l0, l1 = lstar(interp(fit, 0.0), ymax), lstar(ymax, ymax)
    pts = []
    for k in range(POINTS):
        target = lstar_inverse(l0 + (l1 - l0) * k / (POINTS - 1), ymax)
        pts.append(round(first_reaching(fit, target) * OUT_MAX))
    pts[0], pts[-1] = 0, OUT_MAX
    for i in range(1, POINTS):
        pts[i] = min(max(pts[i], pts[i - 1]), OUT_MAX)
    return pts, fit


def predicted(pts, fit, levels=(1, 8, 16, 32, 64, 96, 128, 160, 192, 224, 254)):
    """L* the curve gives at some levels (the fit's light at the curve's output)."""
    ymax = interp(fit, 1.0)
    out = []
    for lv in levels:
        p = (lv - 1) / 253 * (POINTS - 1)
        i = min(int(p), POINTS - 2)
        f = (pts[i] + (pts[i + 1] - pts[i]) * (p - i)) / OUT_MAX
        out.append((lv, lstar(interp(fit, f), ymax)))
    return out


def evenness(lv_l):
    """rms deviation of L* from the straight line between its ends."""
    (a0, l0), (a1, l1) = lv_l[0], lv_l[-1]
    dev = [L - (l0 + (l1 - l0) * (lv - a0) / (a1 - a0)) for lv, L in lv_l]
    return (sum(d * d for d in dev) / len(dev)) ** 0.5


def calibrated(cal):
    """A reading -> lux function from [reading, lux] pairs taken with a light meter: ln(lux)
    fitted non-decreasing over the reading (a light meter is noisy near its floor), interpolated
    between, the ends extrapolated with their slope."""
    fit = monotone([(float(r), math.log(max(float(lx), 1e-6))) for r, lx in cal])
    if len(fit) < 2:
        raise ValueError("a calibration needs at least two different readings")

    def f(r):
        if r <= fit[0][0]:
            (x1, y1), (x2, y2) = fit[0], fit[1]
        elif r >= fit[-1][0]:
            (x1, y1), (x2, y2) = fit[-2], fit[-1]
        else:
            (x1, y1), (x2, y2) = next(((a, b) for a, b in zip(fit, fit[1:]) if a[0] <= r <= b[0]))
        return math.exp(y1 + (y2 - y1) * (r - x1) / (x2 - x1)) if x2 > x1 else math.exp(y2)
    return f


def measure_levels(n):
    """n levels from 1 to 254, closer together at the bottom (where a lamp changes most)."""
    lv = sorted({max(1, min(254, round(1 + 253 * (i / (n - 1)) ** 2))) for i in range(n)})
    return lv


def self_test():
    # a made-up lamp: nothing below a fifth of the conduction, then a soft knee and saturation
    lamp = lambda f: max(0.0, math.tanh(3.0 * (f + 0.05)) - math.tanh(0.15)) ** 1.3
    samples = [((lv - 1) / 253, lamp((lv - 1) / 253)) for lv in measure_levels(14)]
    pts, fit = make_curve(samples)
    assert pts[0] == 0 and pts[-1] == OUT_MAX and all(b >= a for a, b in zip(pts, pts[1:])), pts
    before = evenness([(lv, lstar(lamp((lv - 1) / 253), lamp(1.0))) for lv in range(1, 255, 8)])
    after = evenness(predicted(pts, fit, tuple(range(1, 255, 8))))
    print(f"self-test: linear curve rms {before:.1f} L*, custom curve rms {after:.1f} L*")
    assert after < before / 3, (before, after)
    cal = calibrated([[0, 0.5], [1000, 5.0], [2000, 50.0]])
    assert abs(cal(1000) - 5.0) < 1e-6 and abs(cal(1500) - math.sqrt(5 * 50)) < 1e-6
    noisy = calibrated([[300, 1.0], [350, 0.9], [600, 2.0], [2000, 20.0]])    # 350 below 300
    assert all(noisy(b) >= noisy(a) for a, b in zip(range(250, 2100, 50), range(300, 2150, 50)))
    print("self-test: ok")


# ---- Zigbee2MQTT through Home Assistant or MQTT ------------------------------------------------

class HaLink:
    """Home Assistant's WebSocket API (stdlib only): MQTT publish and subscribe."""

    def __init__(self, url, token):
        u = urlparse(url)
        port = u.port or (443 if u.scheme == "https" else 80)
        self.sock = socket.create_connection((u.hostname, port), timeout=20)
        if u.scheme == "https":
            import ssl
            self.sock = ssl.create_default_context().wrap_socket(self.sock, server_hostname=u.hostname)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET /api/websocket HTTP/1.1\r\nHost: {u.hostname}:{port}\r\n"
                           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise IOError("Home Assistant closed the connection")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n")[0] + b" ":
            raise IOError("no WebSocket from Home Assistant: " + head.split(b"\r\n")[0].decode(errors="replace"))
        self.n = 0
        self._recv()                                   # auth_required
        self._send({"type": "auth", "access_token": token})
        if self._recv().get("type") != "auth_ok":
            raise IOError("Home Assistant refused the token")
        self.queue = []

    def _send(self, obj):
        data = json.dumps(obj).encode()
        head = bytearray([0x81])
        n = len(data)
        head += bytes([0x80 | n]) if n < 126 else (bytes([0x80 | 126]) + struct.pack(">H", n)
                                                   if n < 65536 else bytes([0x80 | 127]) + struct.pack(">Q", n))
        mask = os.urandom(4)
        self.sock.sendall(bytes(head) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise IOError("Home Assistant closed the connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _recv(self):
        data = b""
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            payload = self._read(n) if n else b""
            op = b0 & 0x0F
            if op == 0x9:                              # ping: pong
                mask = os.urandom(4)
                self.sock.sendall(bytes([0x8A, 0x80 | len(payload)]) + mask +
                                  bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
                continue
            if op == 0x8:
                raise IOError("Home Assistant closed the connection")
            data += payload
            if b0 & 0x80:
                return json.loads(data)

    def subscribe(self, topic):
        self.n += 1
        self._send({"id": self.n, "type": "mqtt/subscribe", "topic": topic})

    def publish(self, topic, payload):
        self.n += 1
        self._send({"id": self.n, "type": "call_service", "domain": "mqtt", "service": "publish",
                    "service_data": {"topic": topic, "payload": json.dumps(payload)}})

    def poll(self, timeout):
        """(topic, payload) of messages arrived within timeout s."""
        out = []
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if not self.buf:
                r, _, _ = select.select([self.sock], [], [], max(0.0, left))
                if not r:
                    return out
            msg = self._recv()
            ev = msg.get("event", {}) if msg.get("type") == "event" else {}
            if "topic" in ev:
                try:
                    out.append((ev["topic"], json.loads(ev["payload"])))
                except ValueError:
                    pass
            if time.time() >= end and not self.buf:
                return out


class MqttLink:
    def __init__(self, host, port, username, password):
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            sys.exit("--mqtt needs paho-mqtt: pip install paho-mqtt (or use --ha)")
        try:
            self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        except AttributeError:                         # paho-mqtt 1.x
            self.c = mqtt.Client()
        if username:
            self.c.username_pw_set(username, password)
        self.queue = []
        self.c.on_message = lambda c, u, m: self.queue.append((m.topic, m.payload))
        self.c.connect(host, port, 30)
        self.c.loop_start()

    def subscribe(self, topic):
        self.c.subscribe(topic)

    def publish(self, topic, payload):
        self.c.publish(topic, json.dumps(payload))

    def poll(self, timeout):
        time.sleep(timeout)
        out, self.queue = self.queue, []
        res = []
        for t, p in out:
            try:
                res.append((t, json.loads(p)))
            except ValueError:
                pass
        return res


class Lamp:
    def __init__(self, link, base, device, channel, sensor, to_lux, manual, hold, log):
        self.link, self.base, self.dev, self.ch = link, base, device, channel
        self.sensor, self.to_lux, self.manual, self.hold, self.log = sensor, to_lux, manual, hold, log
        self.state, self.lux = {}, []
        link.subscribe(f"{base}/{device}")
        if sensor:
            link.subscribe(f"{base}/{sensor}")

    def pump(self, s):
        for topic, p in self.link.poll(s):
            if not isinstance(p, dict):
                continue
            if topic == f"{self.base}/{self.dev}":
                self.state.update(p)
            elif self.sensor and topic == f"{self.base}/{self.sensor}":
                v = p.get("illuminance", p.get("illuminance_lux"))
                if isinstance(v, (int, float)):
                    self.lux.append((time.time(), float(v)))

    def set(self, payload):
        self.link.publish(f"{self.base}/{self.dev}/set", payload)

    def get(self, key, timeout=6.0):
        self.state.pop(key, None)
        self.link.publish(f"{self.base}/{self.dev}/get", {key: ""})
        end = time.time() + timeout
        while time.time() < end and key not in self.state:
            self.pump(0.3)
        return self.state.get(key)

    def wait_state(self, want, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end and self.state.get(f"state_{self.ch}") != want:
            self.pump(0.2)

    def read(self, what, window):
        """The light now: the sensor's last report in the window (a fresh one if any), or typed."""
        if self.manual:
            self.pump(window)
            while True:
                text = input(f"  {what}: light meter reading (lx): ").strip().replace(",", ".")
                try:
                    return float(text)
                except ValueError:
                    print("  a number, please")
        start = time.time()
        self.pump(window)
        fresh = [v for t, v in self.lux if t >= start]
        old = [v for t, v in self.lux if t < start]
        r = fresh[-1] if fresh else (old[-1] if old else None)
        if r is None:
            raise SystemExit("no illuminance from the sensor: is it on and reporting?")
        return self.to_lux(r)

    def measure(self, levels):
        rows = []
        for i, lv in enumerate(levels, 1):
            self.set({f"state_{self.ch}": "OFF"})
            self.wait_state("OFF")
            dark = self.read(f"[{i}/{len(levels)}] dark", self.hold / 2)
            self.set({f"brightness_{self.ch}": lv, "transition": 0})
            self.wait_state("ON")
            lit = self.read(f"[{i}/{len(levels)}] level {lv}", self.hold)
            rows.append([lv, lit, dark])
            self.log(f"  level {lv:3}: {lit:.3f} lx (dark {dark:.3f})")
        self.set({f"state_{self.ch}": "OFF"})
        return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--ha", help="Home Assistant URL (token in HA_TOKEN)")
    ap.add_argument("--mqtt", help="MQTT broker host (password in MQTT_PASSWORD)")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--username")
    ap.add_argument("--base", default="zigbee2mqtt", help="Zigbee2MQTT's base topic")
    ap.add_argument("--device", help="the dimmer's Zigbee2MQTT name")
    ap.add_argument("--channel", default="l1", help="l1 or l2")
    ap.add_argument("--sensor", help="an illuminance sensor's Zigbee2MQTT name")
    ap.add_argument("--manual", action="store_true", help="type the readings of a light meter")
    ap.add_argument("--calibration", help="JSON [[reading, lux], ...] for a sensor that is not linear")
    ap.add_argument("--levels", type=int, default=14, help="how many levels to measure")
    ap.add_argument("--hold", type=float, default=12.0, help="s at each level (the sensor reports)")
    ap.add_argument("--apply", action="store_true", help="write the curve and select it")
    ap.add_argument("--save", help="save the measurement and the curve (JSON)")
    ap.add_argument("--load", help="compute the curve from a saved measurement instead")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()

    if a.load:
        run = json.load(open(a.load))
    else:
        if not a.device or not (a.sensor or a.manual) or not (a.ha or a.mqtt):
            ap.error("give --device, --sensor or --manual, and --ha or --mqtt")
        if a.ha:
            token = os.environ.get("HA_TOKEN") or sys.exit("set HA_TOKEN (a long-lived access token)")
            link = HaLink(a.ha, token)
        else:
            link = MqttLink(a.mqtt, a.port, a.username, os.environ.get("MQTT_PASSWORD"))
        to_lux = calibrated(json.load(open(a.calibration))) if a.calibration else (lambda r: r)
        lamp = Lamp(link, a.base, a.device, a.channel, a.sensor, to_lux, a.manual, a.hold, print)
        ch = a.channel
        settings = {k: lamp.get(f"{k}_{ch}") for k in ("dimming_curve", "min_level", "max_level")}
        print(f"{a.device} {ch}: {settings}")
        if settings["dimming_curve"] is None:
            sys.exit("the dimmer did not answer (name, channel, Open Dimmer 2.2.8 on it?)")
        levels = measure_levels(a.levels)
        print(f"measuring {len(levels)} levels with the linear curve, each from dark "
              f"(about {len(levels) * a.hold * 1.6 / 60:.0f} min): keep the room dark and still")
        lamp.set({f"dimming_curve_{ch}": "linear"})
        try:
            rows = lamp.measure(levels)
        finally:
            lamp.set({f"dimming_curve_{ch}": settings["dimming_curve"]})
            lamp.set({f"state_{ch}": "OFF"})
        run = {"device": a.device, "channel": ch, "settings": settings, "rows": rows,
               "time": time.strftime("%Y-%m-%d %H:%M")}

    samples = [((lv - 1) / 253, lit - dark) for lv, lit, dark in run["rows"]]
    pts, fit = make_curve(samples)
    run["curve"] = pts
    before = evenness([(lv, lstar(max(lit - dark, 0), interp(fit, 1.0))) for lv, lit, dark in run["rows"]])
    pred = predicted(pts, fit)
    print("\ncustom_curve: " + ",".join(map(str, pts)))
    print(f"perceived lightness, rms off even steps: linear {before:.1f}, this curve {evenness(pred):.1f} (predicted)")
    print("  level " + " ".join(f"{lv:5}" for lv, _ in pred))
    print("  L*    " + " ".join(f"{L:5.1f}" for _, L in pred))
    if a.save:
        json.dump(run, open(a.save, "w"), indent=1)
        print(f"saved {a.save}")
    if a.apply:
        if a.load:
            sys.exit("--apply needs a connection: run without --load (or write custom_curve yourself)")
        ch = run["channel"]
        lamp.set({f"custom_curve_{ch}": ",".join(map(str, pts))})
        lamp.pump(1.5)
        lamp.set({f"dimming_curve_{ch}": "custom"})
        lamp.pump(1.5)
        got = lamp.get(f"custom_curve_{ch}"), lamp.get(f"dimming_curve_{ch}")
        print(f"written: custom_curve_{ch} {got[0]}, dimming_curve_{ch} {got[1]}")
        if got[1] != "custom" or not got[0]:
            sys.exit("the dimmer did not take it (Open Dimmer 2.2.8 and its converter?)")
        zone = lamp.get(f"hold_slow_zone_{ch}")
        if zone:
            print(f"tip: hold_slow_zone_{ch} is {zone}: with a custom curve 0 gives an even hold speed")


if __name__ == "__main__":
    main()
