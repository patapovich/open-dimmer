#!/usr/bin/env python3
"""Synthetic app images for the bootloader harness (make test-boot). No SDK needed.

Each image is what tools/mkimage.py would make of an app: a vector table (SP 0x20000BE0, odd
vectors inside the image), the 16-byte header at 0xC0 (src/ota_proto.h: magic, size, CRC with
its own field read as 0, version byte, flags, build) and code bytes. The code bytes come from a
seeded generator, so every image is the same on every run. Page 0 holds only vectors and is the
same in every normal image, as in real builds; "variants" of one base differ in chosen pages.

Some images break one rule on purpose (bench flags, a bad SP, a wrong magic, sizes at and past
the layout limits): the bootloader must refuse them, whatever a ZTU serves.

Usage: mkimages.py <output dir>"""
import hashlib
import os
import struct
import sys
import zlib

APP_BASE = 0x08001000
HEADER = 0xC0
MAGIC = 0x5A445041
SP = 0x20000BE0
PAGE = 128
CONFIRMS, RING_HIGH = 0x08, 0x10
SIM, MEASURE, DIAG = 0x01, 0x02, 0x04


def stream(seed, n):
    out = bytearray()
    k = 0
    while len(out) < n:
        out += hashlib.sha256(f"{seed}:{k}".encode()).digest()
        k += 1
    return out[:n]


def image(size, version, flags, build, base="base", changed=None, sp=SP, reset=None, magic=MAGIC):
    """changed: page numbers whose code bytes come from another seed than the base's (None: all
    pages but page 0 differ, as between unrelated builds)."""
    d = bytearray(stream(base, size))
    if changed is None:
        d = bytearray(stream(f"{base}/{build}/{flags}/{size}", size))
    else:
        alt = stream(f"{base}/v{build}", size)
        for p in changed:
            d[p * PAGE:(p + 1) * PAGE] = alt[p * PAGE:(p + 1) * PAGE]
    reset = APP_BASE + 0x101 if reset is None else reset
    vectors = [sp, reset] + [APP_BASE + 0x105 + 4 * i for i in range(46)]
    struct.pack_into("<48I", d, 0, *vectors)
    struct.pack_into("<IIIBBH", d, HEADER, magic, size, 0, version, flags, build)
    crc = zlib.crc32(bytes(d)) & 0xFFFFFFFF
    struct.pack_into("<I", d, HEADER + 8, crc)
    return bytes(d)


def crc0(size, version, flags, build):
    """An image whose CRC (with its field read as 0) is 0: the last 4 bytes are solved for it.
    CRC-32 is affine over GF(2), so the 32 bits that zero it come from a 32x32 linear system."""
    d = bytearray(image(size, version, flags, build, base="crc0"))
    struct.pack_into("<I", d, HEADER + 8, 0)
    tail = size - 4

    def crc_with(x):
        struct.pack_into("<I", d, tail, x)
        return zlib.crc32(bytes(d)) & 0xFFFFFFFF

    base = crc_with(0)
    cols = [crc_with(1 << i) ^ base for i in range(32)]
    # solve sum(x_i * cols[i]) = base over GF(2) by elimination
    basis = []                                    # (vector, combination of bits)
    for i, c in enumerate(cols):
        comb = 1 << i
        for v, cb in basis:
            if c ^ v < c:
                c, comb = c ^ v, comb ^ cb
        if c:
            basis.append((c, comb))
            basis.sort(reverse=True)
    target, comb = base, 0
    for v, cb in basis:
        if target ^ v < target:
            target, comb = target ^ v, comb ^ cb
    assert target == 0, "no solution"
    assert crc_with(comb) == 0
    return bytes(d)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    half = list(range(1, 91, 2))           # every other page differs (page 1 holds the header)
    imgs = {
        # normal releases and test builds (CONFIRMS); 300..302 share a base, 299 does not
        "n299": image(11476, 0x9D, CONFIRMS, 299, base="old"),
        "n300": image(11628, 0x9E, CONFIRMS, 300, changed=[1]),
        "n301": image(11628, 0x9F, CONFIRMS, 301, changed=half),
        "n302": image(11628, 0xA0, CONFIRMS, 302, changed=[1, 2, 40, 41, 90]),
        "n303r": image(11628, 0xA1, CONFIRMS | RING_HIGH, 303, changed=[1, 3]),
        "big400": image(13312, 0xA2, CONFIRMS | RING_HIGH, 400),
        "big401n": image(13312, 0xA3, CONFIRMS, 401),             # > 12 KB without RING_HIGH
        "max304": image(0x3000, 0xA4, CONFIRMS, 304),             # exactly 12 KB
        "over305": image(0x3004, 0xA5, CONFIRMS, 305),            # 12 KB + 4
        "max406r": image(0x3800, 0xA6, CONFIRMS | RING_HIGH, 406),  # exactly 14 KB
        "over407r": image(0x3804, 0xA7, CONFIRMS | RING_HIGH, 407),  # 14 KB + 4: never
        # 2.1.x: no build, no CONFIRMS
        "v2_1_10": image(11628, 0x9A, 0, 0, base="v2110"),
        "v2_1_11": image(11628, 0x9B, 0, 0, base="v2111"),
        "v2_1_12": image(11628, 0x9C, 0, 0, base="v2112"),
        # bench builds (CONFIRMS too, as v2 bench builds have it)
        "sim306": image(11628, 0xBF, SIM | CONFIRMS, 0xFFFE),
        "measure308": image(11628, 0x8F, MEASURE | CONFIRMS, 308),
        "diag305": image(11628, 0x8E, DIAG | CONFIRMS, 305),
        "diag307": image(11628, 0x8E, DIAG | CONFIRMS, 307),
        # rules broken on purpose
        "badsp301": image(11628, 0x9F, CONFIRMS, 301, sp=0x30000BE0),
        "badrv301": image(11628, 0x9F, CONFIRMS, 301, reset=APP_BASE + 11628 + 1),
        "magic301": image(11628, 0x9F, CONFIRMS, 301, magic=MAGIC ^ 0xFF),
        "bffff": image(11628, 0xA8, CONFIRMS, 0xFFFF),            # build 0xFFFF: never made
        "tiny": image(0xD0, 0xA9, CONFIRMS, 309, reset=APP_BASE + 0xC1),   # the smallest valid
        "crc0": crc0(11628, 0xAA, CONFIRMS, 310),                  # CRC 0: "no image" inside the bootloader
    }
    for name, d in imgs.items():
        with open(os.path.join(out, name + ".bin"), "wb") as f:
            f.write(d)


if __name__ == "__main__":
    main()
