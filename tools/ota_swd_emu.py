#!/usr/bin/env python3
# Play the Zigbee module over SWD: trigger an update in the running app, then feed the
# bootloader blocks through its injection buffer. Usage: ota_sim.py <app.img> <running app elf> [stop_at_bytes]
import os, struct, subprocess, sys, time
from pyocd.core.helpers import ConnectHelper
FW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # this repo
def syms(elf):
    out = subprocess.run(["arm-none-eabi-nm", "-S", elf], capture_output=True, text=True).stdout
    d = {}
    for l in out.splitlines():
        p = l.split()
        if len(p) == 4: d.setdefault(p[3], []).append((int(p[0], 16), int(p[1], 16)))
    return d
def frame(cmd, data):
    f = bytes([0x55, 0xAA, 0x02, 0, 0, cmd, len(data) >> 8, len(data) & 0xFF]) + data
    return f + bytes([sum(f) & 0xFF])
img = open(sys.argv[1], "rb").read(); app = syms(sys.argv[2]); boot = syms(FW + "/build-boot/boot.elf")
stop = int(sys.argv[3]) if len(sys.argv) > 3 else None
size, crc, ver = struct.unpack_from("<I", img, 0xC4)[0], struct.unpack_from("<I", img, 0xC8)[0], img[0xCC]
RXb, RXs = [x for x in app["g_rx"] if x[1] == 0x80][0]; HEAD = [x for x in app["g_head"] if x[0] > RXb - 8][0][0]
INJ, INJL, INJP, BOOT = boot["g_inject"][0][0], boot["g_inject_len"][0][0], boot["g_inject_pos"][0][0], boot["g_boot"][0][0]
opts = {"target_override": "py32f002ax5", "pack": os.path.join(FW, "..", "pack", "Puya.PY32F0xx_DFP.1.1.7-opt.pack"), "frequency": 400000, "connect_mode": "attach"}
with ConnectHelper.session_with_chosen_probe(options=opts) as s:
    t = s.board.target
    def bstate():
        b = t.read_memory_block8(BOOT, 32)
        exp, sz, cr = struct.unpack_from("<III", bytes(b), 0); v, res, er, valid = b[12:16]; req, blk, pg = struct.unpack_from("<III", bytes(b), 16)
        return dict(expect=exp, size=sz, version=v, result=res, erased=er, app_valid=valid, requests=req, blocks=blk, pages=pg)
    if stop != -1:
        f = frame(0xA0, bytes([ver]) + struct.pack(">II", size, crc)); h = t.read8(HEAD)
        for i, b in enumerate(f): t.write8(RXb + ((h + i) & 0x7F), b)
        t.write8(HEAD, (h + len(f)) & 0x7F); t.flush(); time.sleep(0.4)
    st = bstate(); print("bootloader:", st); t0 = time.time(); sent = 0
    while True:
        if t.read32(0xE000ED08) != 0: print("app running (VTOR moved)"); break
        st = bstate(); off = st["expect"]
        if stop is not None and stop > 0 and off >= stop: print(f"stopping at {off} bytes (simulated power cut)"); break
        n = min(64, 128 - (off % 128), size - off)
        f = frame(0xA1, struct.pack(">III", off, size, crc) + bytes([ver]) + img[off:off + n])
        t.write16(INJL, 0); t.write_memory_block8(INJ, list(f)); t.write16(INJP, 0); t.write16(INJL, len(f)); t.flush()
        t1 = time.time()
        while t.read16(INJP) < len(f):
            if time.time() - t1 > 2 or t.read32(0xE000ED08) != 0: break
        sent += 1
        if time.time() - t0 > 120: print("timeout"); break
    time.sleep(0.5)
    if stop and stop > 0:
        print("before reset:", bstate()); t.reset(); time.sleep(0.6)
        print("after reset: VTOR", hex(t.read32(0xE000ED08)), bstate())
    print(f"blocks sent {sent} in {time.time()-t0:.1f} s; bootloader after:", bstate())
    print("flash header:", t.read_memory_block32(0x080008C0, 4), " VTOR", hex(t.read32(0xE000ED08)))
