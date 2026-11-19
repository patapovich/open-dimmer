#!/usr/bin/env python3
"""Fill size and CRC into the app header (ota_proto.h app_header_t at +0xC0) and check the image
against the rules of docs/BOOTLOADER.md (sections 1, 2 and 11).

    mkimage.py app.bin app.img [--test N]
        app image, as written at 0x08001000. --test N (1..99, make FW_TEST=N): a spare test
        build, whose build number must be the release build + N (with make FW_PREV=1: the
        previous release build + N)
    mkimage.py app.img --full boot.bin out.bin [--old-boot]
        boot padded to 4 KB + app: the whole flash image for an SWD install. The bootloader
        must have its boot_id at 0x10 (--old-boot: a v1 bootloader, bench only). The app must
        follow the rules below (a 2.1.x image, build 0, is refused)
The CRC is CRC-32 (zlib) of the whole image with the crc field read as 0, and never 0.

Header: version byte (Tuya packing, 2 bits major, 2 minor, 4 patch), flags (0x01 SIM, 0x02
MEASURE, 0x04 DIAG: the bench class, which the module never carries, tools/embed_ztu.py refuses
them; 0x08 CONFIRMS; 0x10 RING_HIGH; 0x20..0x80 reserved, refused), build (u16).

Refused for an image this makes: build 0, 1..255 or 0xFFFF; 0xFFFE unless SIM, SIM without
0xFFFE; a build without CONFIRMS; a build that is not a release build (a multiple of 100) or,
with --test N, not the release build + N; CRC 0; over 12 KB without RING_HIGH, over 14 KB;
reserved flags; version byte 0xFF, 0xBF unless SIM, below 0x80 (major < 2 is the stock
firmware's); vectors other than the ZTU's image_info takes (initial SP exactly 0x20000BE0,
so that the stack stays below the mailbox; an odd reset vector inside the image). --full
applies the same rules to the app it takes (the build rules too, but not the release/test
offset). The version byte, which the app and old ZTUs compare as
'strictly newer', gets a warning when its free codes run out.
"""
import os
import struct
import sys

sys.dont_write_bytecode = True                         # no __pycache__ in tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fwimage as fw                                   # noqa: E402

CODES_WARN = 16   # warn when fewer version codes than this are left


def refuse(msg):
    raise fw.Refused(msg)


STOCKLIKE = False   # --stocklike: the bench test T5 build (check_version)


def check_version(version: int, flags: int, stocklike: bool = False) -> None:
    """Refuse the reserved version bytes; warn when the free codes run out. stocklike: the bench
    build of test T5 (make STOCKLIKE_TEST=1 FW_TEST=n) may report a 1.x version: it is flashed with
    a probe only (embed_ztu.py, which the module's image goes through, keeps refusing 1.x)."""
    if stocklike and version < fw.VERSION_OURS_MIN:
        return
    p = fw.version_problem(version, flags)
    if p:
        refuse(p)
    if flags & fw.FLAG_SIM:
        return
    limit = fw.VERSION_SIM if version < fw.VERSION_SIM else fw.VERSION_NEVER
    left = limit - version - 1
    if left < CODES_WARN:
        print(f"warning: version 0x{version:02X} leaves {left} version codes before 0x{limit:02X}"
              + (" (SIM): plan the step to 3.0.0 (0xC0)" if limit == fw.VERSION_SIM
                 else ": the 8-bit version byte is nearly used up"), file=sys.stderr)


def check_app(img: bytes, h, test=0) -> None:
    """The rules for an app image (header filled in), the same for an image it creates and for
    one --full takes (fwimage.app_problem: reserved flags, layout, SP exactly 0x20000BE0 and an
    odd reset vector inside the image, version byte, build). test: the FW_TEST offset its build
    must have (0: a release build), None for an existing image (--full)."""
    if h.build == fw.BUILD_NONE:
        refuse("app header without a build number (build 0): FW_BUILD in src/tuya.h; a 2.1.x "
               "image does not go into a full image")
    p = fw.app_problem(img, h, STOCKLIKE)
    if p:
        refuse(p)
    if (test is not None and h.build != fw.BUILD_SIM
            and h.build % fw.BUILD_STEP != test):
        refuse(f"build {h.build}: " + (
            f"--test {test} wants the release build + {test} (FW_PREV=1: the previous one's + {test})"
            if test else "a build without FW_TEST must be a release build (a multiple of 100); "
                         "test builds: make FW_TEST=1..99"))
    check_version(h.version, h.flags, STOCKLIKE)


def summary(h, size: int) -> str:
    flags = h.flags
    return (f"app image: {size} bytes, crc {h.crc:08x}, {fw.describe(h)}"
            + (", SIM BENCH BUILD: never on mains" if flags & fw.FLAG_SIM else "")
            + (", MEASUREMENT BUILD: outputs off, takes no update (back over SWD)"
               if flags & fw.FLAG_MEASURE else "")
            + (", DIAGNOSTICS BUILD: takes no update (back over SWD)" if flags & fw.FLAG_DIAG else ""))


def make_app(data: bytes, test: int = 0) -> bytes:
    data = bytearray(data)
    while len(data) % 4:
        data.append(0xFF)
    if len(data) < fw.APP_SIZE_MIN:
        refuse(f"app of {len(data)} bytes: shorter than its header")
    h = fw.header(data)
    if h.magic != fw.APP_MAGIC:
        refuse(f"no app header at 0x{fw.APP_HEADER_OFFSET:X} (magic {h.magic:#x})")
    struct.pack_into("<II", data, fw.APP_HEADER_OFFSET + 4, len(data), 0)
    crc = fw.image_crc(data)
    struct.pack_into("<I", data, fw.APP_HEADER_OFFSET + 8, crc)
    h = fw.header(data)
    print(summary(h, len(data)), file=sys.stderr)
    if crc == 0:
        refuse("image CRC 0: the bootloader uses 0 for 'no image'; change any byte (e.g. FW_BUILD)")
    check_app(bytes(data), h, test=test)
    return bytes(data)


def make_full(app: bytes, boot: bytes, old_boot: bool = False) -> bytes:
    if len(boot) > fw.BOOT_SIZE:
        refuse(f"bootloader is {len(boot)} bytes, max {fw.BOOT_SIZE}")
    bid, crc, kind = fw.describe_boot(boot)
    if bid is None and not old_boot:
        refuse("bootloader without boot_id at 0x10 (v1 or older): --old-boot for a bench image with it")
    if bid is not None and old_boot:
        refuse("--old-boot, but the bootloader has a boot_id")
    p = fw.boot_vector_problem(boot)
    if p:
        refuse(p)
    if bid is not None:
        p = fw.boot_id_problem(bid)
        if p:
            refuse(f"bootloader identity: {p}")
    h = fw.finished(app)
    print(summary(h, len(app)), file=sys.stderr)
    check_app(app, h, test=None)
    ceiling = fw.APP_LIMIT_LEGACY if bid is None else bid["app_max"]
    if len(app) > ceiling:
        refuse(f"app of {len(app)} bytes, the bootloader takes {ceiling}")
    print(f"bootloader: {kind}, {len(boot)} bytes; boot sector CRC-32 0x{crc:08X} (DP 136)",
          file=sys.stderr)
    return boot + b"\xff" * (fw.BOOT_SIZE - len(boot)) + app


def parse(argv):
    args, opts, i = [], {}, 1
    while i < len(argv):
        a = argv[i]
        if a in ("--full", "--test"):
            if i + 1 >= len(argv):
                sys.exit(__doc__)
            opts[a] = argv[i + 1]
            i += 2
            continue
        if a in ("--old-boot", "--stocklike"):
            opts[a] = True
        elif a.startswith("--"):
            sys.exit(f"unknown option {a}\n{__doc__}")
        else:
            args.append(a)
        i += 1
    return args, opts


def main(argv):
    global STOCKLIKE
    args, opts = parse(argv)
    STOCKLIKE = "--stocklike" in opts
    if STOCKLIKE and "--test" not in opts and "--full" not in opts:
        sys.exit("--stocklike is a test build's (make STOCKLIKE_TEST=1 FW_TEST=n)")
    try:
        if len(args) == 2 and "--full" not in opts and "--old-boot" not in opts:
            test = 0
            if "--test" in opts:
                if not opts["--test"].isdigit() or not 1 <= int(opts["--test"]) <= 99:
                    refuse(f"--test {opts['--test']}: a test build takes 1..99")
                test = int(opts["--test"])
            with open(args[0], "rb") as f:
                img = make_app(f.read(), test)   # before the output exists: a refusal
            with open(args[1], "wb") as f:       # leaves no empty image
                f.write(img)
        elif len(args) == 2 and "--full" in opts and "--test" not in opts:
            with open(args[0], "rb") as f:
                app = f.read()
            with open(opts["--full"], "rb") as f:
                boot = f.read()
            full = make_full(app, boot, "--old-boot" in opts)
            with open(args[1], "wb") as f:
                f.write(full)
        else:
            sys.exit(__doc__)
    except fw.Refused as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main(sys.argv)
