#!/usr/bin/env python3
"""Minimal Home Assistant websocket client (stdlib only): send commands, print results.
Usage: ha_ws.py [--json] '<json command without id>' ...
  HA_URL and HA_TOKEN come from the file $HA_ENV (default $HA_HELPERS/.env when HA_HELPERS is
  set, else ~/ha/.env).
  Default output: one summary line per command. --json: each command's full result as one JSON
  line on stdout (e.g. for config/device_registry/list); a failed command goes to stderr, exit 1."""
import base64, json, os, socket, ssl, struct, sys, urllib.parse

args = sys.argv[1:]
raw = bool(args) and args[0] == "--json"
if raw:
    args = args[1:]
env_file = os.environ.get("HA_ENV") or os.path.join(os.environ.get("HA_HELPERS") or "~/ha", ".env")
env = dict(l.strip().split("=", 1) for l in open(os.path.expanduser(env_file)) if "=" in l and not l.startswith("#"))
u = urllib.parse.urlparse(env["HA_URL"])
host, port = u.hostname, u.port or (443 if u.scheme == "https" else 80)
sock = socket.create_connection((host, port), timeout=15)
if u.scheme == "https":
    sock = ssl.create_default_context().wrap_socket(sock, server_hostname=host)
key = base64.b64encode(os.urandom(16)).decode()
sock.sendall((f"GET /api/websocket HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf:
    buf += sock.recv(4096)
head, buf = buf.split(b"\r\n\r\n", 1)
assert b" 101 " in head.split(b"\r\n")[0], head[:80]

def send(obj):
    data = json.dumps(obj).encode()
    mask = os.urandom(4)
    hdr = bytes([0x81])
    n = len(data)
    hdr += bytes([0x80 | n]) if n < 126 else (bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536 else bytes([0x80 | 127]) + struct.pack(">Q", n))
    sock.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

def recv():
    global buf
    def need(k):
        global buf
        while len(buf) < k:
            chunk = sock.recv(65536)
            if not chunk:
                raise EOFError
            buf += chunk
    msg = b""
    while True:
        need(2)
        b1, b2 = buf[0], buf[1]
        n, off = b2 & 0x7F, 2
        if n == 126:
            need(4); n = struct.unpack(">H", buf[2:4])[0]; off = 4
        elif n == 127:
            need(10); n = struct.unpack(">Q", buf[2:10])[0]; off = 10
        need(off + n)
        msg += buf[off:off + n]
        buf = buf[off + n:]
        if b1 & 0x80:
            return json.loads(msg)

assert recv()["type"] == "auth_required"
send({"type": "auth", "access_token": env["HA_TOKEN"]})
assert recv()["type"] == "auth_ok"
failed = False
for i, arg in enumerate(args, start=1):
    cmd = json.loads(arg); cmd["id"] = i
    send(cmd)
    while True:
        r = recv()
        if r.get("id") == i and r.get("type") == "result":
            if not raw:
                print(json.dumps({"cmd": cmd.get("entity_id", cmd["type"]), "success": r["success"],
                                  "result": (r.get("result") or {}).get("entity_entry", {}).get("entity_id") if isinstance(r.get("result"), dict) else None,
                                  "error": r.get("error")}))
            elif r["success"]:
                print(json.dumps(r.get("result")))
            else:
                print(json.dumps({"cmd": cmd["type"], "error": r.get("error")}), file=sys.stderr)
                failed = True
            break
sys.exit(1 if failed else 0)
