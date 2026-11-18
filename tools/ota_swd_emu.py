#!/usr/bin/env python3
"""Play the Zigbee module (ZTU) over SWD, for bench tests of the update path:

    ota_swd_emu.py <app.img> <running app elf> [stop_at_bytes]

1. Offer: puts an 0xA0 offer of app.img into the running app's receive ring (the v2 format of
   11 bytes, with the build, so that build-only test images are taken; a 2.1.x app reads the
   first 9). The app accepts it and restarts into the bootloader. stop_at_bytes = -1: no offer
   (the bootloader already listens, e.g. no valid app).
2. Serve: answers the bootloader's 0xA1 block requests through its SWD injection buffer
   (g_inject), statelessly like the ZTU's answer_block: offset, size, CRC, version byte, then
   up to min(max_len, 64) bytes of the image. A v2 bootloader (boot.elf with k_boot_id) asks
   for the header first (offset 0xC0, 16 bytes) and sends 16-byte requests: each one is read
   from its request buffer g_req when its request counter moves. A v1 bootloader is fed the
   next block at its g_boot.expect, as before.
3. stop_at_bytes > 0: stops once the bootloader asks for that offset (a simulated power cut)
   and resets the target. Before every reset it issues, the boot word pair (mailbox 0x18 and
   0x1C, 0x20000BF8 / 0x20000BFC) is written 0, so the next boot starts with fresh tries.
Needs build-boot/boot.elf of the installed bootloader (its symbols) and pyOCD.
"""
import os
import struct
import subprocess
import sys
import time

FW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # this repo
VTOR = 0xE000ED08
BOOT_WORD, BOOT_CHECK = 0x20000BF8, 0x20000BFC
BLOCK_MAX = 64              # what the ZTU serves (fork BLOCK_MAX); the bootloader takes up to 128
HDR_OFF, HDR_LEN = 0xC0, 16
REQ_LEN_V2 = 16
# g_boot (bootloader state for SWD), the v1 layout in both: expect, size, crc (u32), version,
# result, erased, app_valid (u8), requests, blocks, pages (u32, at 16, 20, 24); v2 adds build
# (u16, 0x1C) and the request mode (u8, 0x1E) behind them
G_REQUESTS, G_BUILD, G_MODE = 16, 0x1C, 0x1E


def syms(elf):
    out = subprocess.run(["arm-none-eabi-nm", "-S", elf], capture_output=True, text=True).stdout
    d = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            d.setdefault(p[3], []).append((int(p[0], 16), int(p[1], 16)))
    return d


def frame(cmd, data, seq=0):
    f = bytes([0x55, 0xAA, 0x02, seq >> 8, seq & 0xFF, cmd, len(data) >> 8, len(data) & 0xFF]) + data
    return f + bytes([sum(f) & 0xFF])


def image_fields(img):
    size, crc, ver, flags, build = struct.unpack_from("<IIBBH", img, HDR_OFF + 4)
    return size, crc, ver, flags, build


def offer(img):
    """0xA0 payload, v2: version(1) size(4 BE) crc(4 BE) build(2 BE)."""
    size, crc, ver, _, build = image_fields(img)
    return bytes([ver]) + struct.pack(">IIH", size, crc, build)


def answer(img, off, max_len):
    """0xA1 answer payload for a request (the ZTU's stateless rule)."""
    size, crc, ver, _, _ = image_fields(img)
    n = 0 if off >= size else min(max_len, BLOCK_MAX, 128, size - off)
    return struct.pack(">III", off, size, crc) + bytes([ver]) + img[off:off + n]


def parse_request(req):
    """offset, max_len, bootloader version, mode of a 16-byte (v2) request buffer."""
    off, max_len = struct.unpack_from(">IH", req, 0)
    return off, max_len, req[6], req[7]


def clear_boot_word(t):
    """Before any reset: a debugger reset keeps the RAM (docs/BOOTLOADER.md section 3)."""
    t.write32(BOOT_WORD, 0)
    t.write32(BOOT_CHECK, 0)


def reset(t):
    clear_boot_word(t)
    t.reset()


class Session:
    """The emulator against a target object (pyOCD's, or the tests' model)."""

    def __init__(self, t, img, app, boot, out=print):
        self.t, self.img, self.out = t, img, out
        self.v2 = "k_boot_id" in boot
        self.INJ, self.INJL, self.INJP = boot["g_inject"][0][0], boot["g_inject_len"][0][0], boot["g_inject_pos"][0][0]
        self.BOOT, self.BOOT_LEN = boot["g_boot"][0]
        if self.v2:
            reqs = [a for a, n in boot.get("g_req", []) if n == REQ_LEN_V2]
            if not reqs:
                sys.exit("boot.elf: no 16-byte g_req (the v2 bootloader's request buffer)")
            self.REQ = reqs[0]
        self.app = app

    def bstate(self):
        b = bytes(self.t.read_memory_block8(self.BOOT, max(28, min(self.BOOT_LEN, 32))))
        exp, sz, cr = struct.unpack_from("<III", b, 0)
        req, blk, pg = struct.unpack_from("<III", b, G_REQUESTS)
        st = dict(expect=exp, size=sz, crc=cr, version=b[12], result=b[13], erased=b[14], app_valid=b[15],
                  requests=req, blocks=blk, pages=pg)
        if len(b) > G_MODE:
            st.update(build=struct.unpack_from("<H", b, G_BUILD)[0], mode=b[G_MODE])
        return st

    def send_offer(self):
        """Into the app's receive ring (g_rx, 128 bytes, and its head index g_head)."""
        t = self.t
        rxb = [x for x in self.app["g_rx"] if x[1] == 0x80][0][0]
        head = [x for x in self.app["g_head"] if x[0] > rxb - 8][0][0]
        f = frame(0xA0, offer(self.img))
        h = t.read8(head)
        for i, b in enumerate(f):
            t.write8(rxb + ((h + i) & 0x7F), b)
        t.write8(head, (h + len(f)) & 0x7F)
        t.flush()

    def inject(self, payload):
        t = self.t
        f = frame(0xA1, payload)
        t.write16(self.INJL, 0)
        t.write_memory_block8(self.INJ, list(f))
        t.write16(self.INJP, 0)
        t.write16(self.INJL, len(f))
        t.flush()
        t1 = time.time()
        while t.read16(self.INJP) < len(f):
            if time.time() - t1 > 2 or t.read32(VTOR) != 0:
                break

    def next_request(self, last, timeout=2.0):
        """v2: the newest request once the bootloader's request counter moved past last."""
        t1 = time.time()
        while time.time() - t1 < timeout:
            n = self.t.read32(self.BOOT + G_REQUESTS)
            if n != last:
                return n, parse_request(bytes(self.t.read_memory_block8(self.REQ, REQ_LEN_V2)))
            if self.t.read32(VTOR) != 0:
                break
        return last, None

    def run(self, stop=None, limit_s=120):
        t, img = self.t, self.img
        size = image_fields(img)[0]
        if stop != -1:
            self.send_offer()
            time.sleep(0.4)
        self.out(f"bootloader ({'v2' if self.v2 else 'v1'}): {self.bstate()}")
        t0, sent, last, stopped = time.time(), 0, None, False
        while True:
            if t.read32(VTOR) != 0:
                self.out("app running (VTOR moved)")
                break
            if self.v2:
                last, req = self.next_request(last)
                if req is None:
                    if time.time() - t0 > limit_s:
                        self.out("timeout")
                        break
                    continue
                off, max_len, _, mode = req
                header_req = off == HDR_OFF and max_len == HDR_LEN
                payload = answer(img, off, max_len)
            else:
                off = self.bstate()["expect"]
                header_req, mode = False, None
                payload = answer(img, off, min(BLOCK_MAX, 128 - (off % 128), size - off))
            if stop is not None and stop > 0 and not header_req and off >= stop:
                self.out(f"stopping at {off} bytes (simulated power cut)")
                stopped = True
                break
            self.inject(payload)
            sent += 1
            if time.time() - t0 > limit_s:
                self.out("timeout")
                break
        time.sleep(0.5)
        if stopped:
            self.out(f"before reset: {self.bstate()}")
            reset(t)
            time.sleep(0.6)
            self.out(f"after reset: VTOR {t.read32(VTOR):#x} {self.bstate()}")
        self.out(f"answers sent {sent} in {time.time() - t0:.1f} s; bootloader after: {self.bstate()}")
        return sent, stopped


def main(argv):
    if len(argv) not in (3, 4):
        sys.exit(__doc__)
    from pyocd.core.helpers import ConnectHelper
    with open(argv[1], "rb") as f:
        img = f.read()
    stop = int(argv[3]) if len(argv) > 3 else None
    app, boot = syms(argv[2]), syms(os.path.join(FW, "build-boot", "boot.elf"))
    opts = {"target_override": "py32f002ax5", "pack": os.path.join(FW, "..", "pack", "Puya.PY32F0xx_DFP.1.1.7-opt.pack"),
            "frequency": 400000, "connect_mode": "attach"}
    with ConnectHelper.session_with_chosen_probe(options=opts) as s:
        t = s.board.target
        t.write32(0x40015808, 0x1000)       # DBG_IWDG_STOP: the v2 bootloader runs the watchdog
        Session(t, img, app, boot).run(stop)
        print("flash header:", t.read_memory_block32(0x080010C0, 4), " VTOR", hex(t.read32(VTOR)))


if __name__ == "__main__":
    main(sys.argv)
