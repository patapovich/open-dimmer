#!/usr/bin/env python3
"""Fill size and CRC into the app header (ota_proto.h app_header_t at +0xC0).

    mkimage.py app.bin app.img            app image, as written at 0x08001000
    mkimage.py app.img --full boot.bin out.bin
                                          boot padded to 4 KB + app: whole flash
                                          image for the first SWD flashing
The CRC is CRC-32 (zlib) of the whole image with the crc field read as 0.

Version bytes (Tuya packing, 2 bits major, 2 minor, 4 patch) the app and the ZTU
compare as 'strictly newer': 0xBF (2.3.15) is reserved for the SIM bench build and
0xFF can never be followed by an update, so no other image may carry them.
"""
import struct, sys, zlib

HDR_OFF, APP_MAGIC, APP_MAX, BOOT_SIZE = 0xC0, 0x5A445041, 12 * 1024, 4096
FLAG_SIM, FLAG_MEASURE = 0x01, 0x02
SIM_VERSION, LAST_VERSION = 0xBF, 0xFF
CODES_WARN = 16   # warn when fewer version codes than this are left

def check_version(version: int, flags: int) -> None:
    """Refuse the reserved version bytes; warn when the free codes run out."""
    if version == LAST_VERSION:
        sys.exit("version byte 0xFF: no update could ever be offered to an app that has it")
    if version == SIM_VERSION and not flags & FLAG_SIM:
        sys.exit("version byte 0xBF (2.3.15) is reserved for the SIM bench build")
    if flags & FLAG_SIM:
        return
    limit = SIM_VERSION if version < SIM_VERSION else LAST_VERSION
    left = limit - version - 1
    if left < CODES_WARN:
        print(f"warning: version 0x{version:02X} leaves {left} version codes before 0x{limit:02X}"
              + (" (SIM): plan the step to 3.0.0 (0xC0)" if limit == SIM_VERSION
                 else ": the 8-bit version byte is nearly used up"), file=sys.stderr)

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
          + (", SIM BENCH BUILD: never on mains" if flags & FLAG_SIM else "")
          + (", MEASUREMENT BUILD: outputs off" if flags & FLAG_MEASURE else ""), file=sys.stderr)
    check_version(v, flags)
    return bytes(data)

if __name__ == "__main__":
    if len(sys.argv) == 3:
        img = make_app(open(sys.argv[1], "rb").read())   # before the output exists: a
        open(sys.argv[2], "wb").write(img)               # refusal leaves no empty image
    elif len(sys.argv) == 5 and sys.argv[2] == "--full":
        app, boot = open(sys.argv[1], "rb").read(), open(sys.argv[3], "rb").read()
        if len(boot) > BOOT_SIZE:
            sys.exit(f"bootloader is {len(boot)} bytes, max {BOOT_SIZE}")
        open(sys.argv[4], "wb").write(boot + b"\xff" * (BOOT_SIZE - len(boot)) + app)
    else:
        sys.exit(__doc__)
