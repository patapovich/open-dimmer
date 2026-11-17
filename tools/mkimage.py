#!/usr/bin/env python3
"""Fill size and CRC into the app header (ota_proto.h app_header_t at +0xC0).

    mkimage.py app.bin app.img            app image, as written at 0x08001000
    mkimage.py app.img --full boot.bin out.bin
                                          boot padded to 4 KB + app: whole flash
                                          image for the first SWD flashing
The CRC is CRC-32 (zlib) of the whole image with the crc field read as 0.
"""
import struct, sys, zlib

HDR_OFF, APP_MAGIC, APP_MAX, BOOT_SIZE = 0xC0, 0x5A445041, 12 * 1024, 4096

def make_app(data: bytes) -> bytes:
    data = bytearray(data)
    while len(data) % 4:
        data.append(0xFF)
    magic, size, crc, version, flags = struct.unpack_from("<IIIBB", data, HDR_OFF)
    if magic != APP_MAGIC:
        sys.exit(f"no app header at 0x{HDR_OFF:X} (magic {magic:#x})")
    if len(data) > APP_MAX:
        sys.exit(f"app is {len(data)} bytes, max {APP_MAX}")
    struct.pack_into("<II", data, HDR_OFF + 4, len(data), 0)
    crc = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    struct.pack_into("<I", data, HDR_OFF + 8, crc)
    v = version
    print(f"app image: {len(data)} bytes, crc {crc:08x}, version {v >> 6}.{(v >> 4) & 3}.{v & 15} (0x{v:02X})"
          + (", SIM BENCH BUILD: never on mains" if flags & 1 else "")
          + (", MEASUREMENT BUILD: outputs off" if flags & 2 else ""), file=sys.stderr)
    return bytes(data)

if __name__ == "__main__":
    if len(sys.argv) == 3:
        open(sys.argv[2], "wb").write(make_app(open(sys.argv[1], "rb").read()))
    elif len(sys.argv) == 5 and sys.argv[2] == "--full":
        app, boot = open(sys.argv[1], "rb").read(), open(sys.argv[3], "rb").read()
        if len(boot) > BOOT_SIZE:
            sys.exit(f"bootloader is {len(boot)} bytes, max {BOOT_SIZE}")
        open(sys.argv[4], "wb").write(boot + b"\xff" * (BOOT_SIZE - len(boot)) + app)
    else:
        sys.exit(__doc__)
