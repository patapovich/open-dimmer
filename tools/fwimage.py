#!/usr/bin/env python3
"""Image rules shared by the tools (src/ota_proto.h v2, docs/BOOTLOADER.md sections 1, 2, 11):
the app header, the bootloader identity (boot_id_t at 0x08000010) and the boot sector CRC.
tools/mkimage.py and tools/embed_ztu.py import it; tests/test_tools.py checks the numbers below
against src/ota_proto.h.

    fwimage.py full <full.bin> [--erase-settings | --keep-settings]
        check a whole-flash image before an SWD install (tools/swd/install.sh): app header and
        CRC, no SIM build, the app rules of every image (reserved flags, layout, SP exactly
        0x20000BE0 and an odd reset vector in the image, version byte, build; no build 0, a
        2.1.x app), the bootloader's identity and boot sector CRC; refuses a bootloader
        without boot_id (v1), an app over 12 KB unless --erase-settings (the plain
        write erases sector 4) or --keep-settings (install.sh reads sector 4 first and writes it
        back with the image's start merged in)
    fwimage.py notes <boot.bin> [<full.bin>]
        the bootloader line of the release notes (version, size, boot sector CRC = DP 136);
        with full.bin also checks that its boot sector is boot.bin padded with 0xFF
"""
import collections
import struct
import sys
import zlib

# ---- numbers of src/ota_proto.h (tests/test_tools.py compares them) -------------------------
BOOT_BASE, BOOT_SIZE, BOOT_ID_OFFSET = 0x08000000, 0x1000, 0x10
APP_BASE, APP_HEADER_OFFSET, APP_SIZE_MIN = 0x08001000, 0xC0, 0xD0
BOOT_APP_MAX, APP_LIMIT_LEGACY = 0x3800, 0x3000          # 14 KB (v2 boot, RING_HIGH), 12 KB
FLASH_SIZE = 0x5000
HIDDEN_BASE, HIDDEN_SIZE = 0x08005000, 0x3000             # v3: the hidden flash (section 18)
BOOT_IMAGE_MAX = BOOT_APP_MAX + HIDDEN_SIZE                # 26 KB: the v3 bootloader's ceiling
APP_MAGIC, BOOT_ID_MAGIC, BOOT_VERSION = 0x5A445041, 0x4C42444F, 3
BOOT_VERSION_MIN = 2                                       # the oldest boot_id a full image takes
FLAG_SIM, FLAG_MEASURE, FLAG_DIAG, FLAGS_BENCH = 0x01, 0x02, 0x04, 0x07
FLAG_CONFIRMS, FLAG_RING_HIGH, FLAGS_RESERVED = 0x08, 0x10, 0xE0
VERSION_OURS_MIN, VERSION_SIM, VERSION_NEVER = 0x80, 0xBF, 0xFF
BUILD_NONE, BUILD_MIN, BUILD_MAX, BUILD_SIM, BUILD_NEVER = 0, 256, 0xFFFD, 0xFFFE, 0xFFFF
BUILD_FIRST, BUILD_STEP = 300, 100
MAILBOX_RAM = 0x20000BE0                                   # also the app's initial SP
BOOT_FEAT_HIDDEN = 0x10
BOOT_FEAT_NAMES = ((0x01, "tries"), (0x02, "probe"), (0x04, "button"), (0x08, "link"),
                   (BOOT_FEAT_HIDDEN, "hidden"))


def image_addr(off):
    """Flash address of image offset off (IMAGE_ADDR of src/ota_proto.h): the first
    BOOT_APP_MAX bytes at APP_BASE, the rest at HIDDEN_BASE past the settings ring."""
    return APP_BASE + off if off < BOOT_APP_MAX else HIDDEN_BASE + off - BOOT_APP_MAX


def main_part(size):
    """The image's bytes in the app area (the rest is hidden)."""
    return min(size, BOOT_APP_MAX)

Header = collections.namedtuple("Header", "magic size crc version flags build")


class Refused(Exception):
    """A rule of docs/BOOTLOADER.md refuses the image; the message says which."""


def header(img):
    if len(img) < APP_SIZE_MIN:
        raise Refused(f"image of {len(img)} bytes: shorter than its header (0x{APP_SIZE_MIN:X})")
    return Header(*struct.unpack_from("<IIIBBH", img, APP_HEADER_OFFSET))


def image_crc(img):
    """CRC-32 (zlib) of the image with the header's CRC field read as 0."""
    d = bytearray(img)
    struct.pack_into("<I", d, APP_HEADER_OFFSET + 8, 0)
    return zlib.crc32(bytes(d)) & 0xFFFFFFFF


def version_str(v):
    return f"{v >> 6}.{(v >> 4) & 3}.{v & 15}"


def flag_names(flags):
    names = [n for b, n in ((FLAG_SIM, "SIM"), (FLAG_MEASURE, "MEASURE"), (FLAG_DIAG, "DIAG"),
                            (FLAG_CONFIRMS, "CONFIRMS"), (FLAG_RING_HIGH, "RING_HIGH")) if flags & b]
    if flags & FLAGS_RESERVED:
        names.append(f"reserved 0x{flags & FLAGS_RESERVED:02X}")
    return "+".join(names) or "none"


def describe(h):
    build = "none (2.1.x)" if h.build == BUILD_NONE else (
        "0xFFFE (SIM)" if h.build == BUILD_SIM else str(h.build))
    return (f"version {version_str(h.version)} (0x{h.version:02X}), build {build}, "
            f"flags 0x{h.flags:02X} ({flag_names(h.flags)})")


def finished(img):
    """Header of a finished image (made by mkimage.py): magic, size == length, CRC, CRC != 0."""
    h = header(img)
    if h.magic != APP_MAGIC or h.size != len(img):
        raise Refused("not an app image made by tools/mkimage.py (header magic or size)")
    if image_crc(img) != h.crc:
        raise Refused("CRC mismatch")
    if h.crc == 0:
        raise Refused("image CRC 0: the bootloader uses 0 for 'no image'")
    return h


def layout_problem(size, flags):
    """The settings layouts (section 1): 12 KB, or with RING_HIGH 14 KB, and up to 26 KB under a
    v3 bootloader (the bytes past 14 KB in the hidden flash, section 18)."""
    if size < APP_SIZE_MIN:
        return f"app of {size} bytes: shorter than its header"
    if size > BOOT_IMAGE_MAX:
        return f"app is {size} bytes, max {BOOT_IMAGE_MAX} (26 KB, BOOT_IMAGE_MAX)"
    if size > APP_LIMIT_LEGACY and not flags & FLAG_RING_HIGH:
        return (f"app is {size} bytes: over {APP_LIMIT_LEGACY} (12 KB) only with RING_HIGH "
                "(settings ring at 0x08004800, the ring-move release first)")
    return None


def vector_problem(img, size, exact_sp):
    """The bootloader's jump check (SP in RAM, reset vector in the image); with exact_sp also the
    ZTU's image_info rule: SP exactly 0x20000BE0 (the mailbox stays outside) and an odd vector."""
    sp, reset = struct.unpack_from("<II", img, 0)
    if exact_sp and sp != MAILBOX_RAM:
        return f"initial SP 0x{sp:08X}, not 0x{MAILBOX_RAM:08X} (end of RAM below the mailbox)"
    if (sp >> 20) != 0x200:
        return f"initial SP 0x{sp:08X} is not in RAM"
    if not APP_BASE <= reset < APP_BASE + main_part(size):
        return (f"reset vector 0x{reset:08X} outside the image's app-area part "
                f"(0x{APP_BASE:08X}..0x{APP_BASE + main_part(size):08X}): not linked for the app at 0x08001000")
    if exact_sp and not reset & 1:
        return f"reset vector 0x{reset:08X} is even (not Thumb)"
    return None


def build_problem(build, flags):
    """Build rules for an image with a build number (section 2, reserved codes); build 0 is the
    caller's case (2.1.x images)."""
    if 1 <= build < BUILD_MIN:
        return f"build {build}: 1..255 are never used"
    if build == BUILD_NEVER:
        return "build 0xFFFF is never used"
    if build == BUILD_SIM and not flags & FLAG_SIM:
        return "build 0xFFFE is reserved for SIM builds"
    if flags & FLAG_SIM and build != BUILD_SIM:
        return f"a SIM build must have build 0xFFFE (nothing is newer), not {build}"
    if build >= BUILD_MIN and not flags & FLAG_CONFIRMS:
        return f"build {build} without the CONFIRMS flag (every app with a build confirms its runs)"
    return None


def version_problem(version, flags):
    if version == VERSION_NEVER:
        return "version byte 0xFF: no update could ever be offered to an app that has it"
    if version == VERSION_SIM and not flags & FLAG_SIM:
        return "version byte 0xBF (2.3.15) is reserved for the SIM bench build"
    if version < VERSION_OURS_MIN:
        return (f"version byte 0x{version:02X} ({version_str(version)}): major below 2 is the stock "
                "firmware's (the ZTU never asks it who it is)")
    return None


def app_problem(img, h, stocklike=False):
    """The rules every app image follows wherever it goes (mkimage, a full image, the ZTU): no
    reserved flag, the settings layout, the exact vectors (SP 0x20000BE0, so that the stack stays
    below the mailbox; an odd reset vector inside the image), the version byte; then the build
    rules (build 0, a 2.1.x app, is refused). stocklike: mkimage's bench build of test T5 (a 1.x
    version byte, flashed with a probe only; every other caller keeps the default)."""
    if h.flags & FLAGS_RESERVED:
        return f"flags 0x{h.flags:02X}: 0x20..0x80 are reserved (the bootloader ignores them)"
    p = (layout_problem(len(img), h.flags) or vector_problem(img, len(img), exact_sp=True)
         or (None if stocklike and h.version < VERSION_OURS_MIN else version_problem(h.version, h.flags)))
    if p:
        return p
    if h.build == BUILD_NONE:
        return ("build 0 (a 2.1.x app): neither installed nor carried (no unit runs one under the "
                "v2 bootloader)")
    return build_problem(h.build, h.flags)


def boot_id(boot):
    """The v2 bootloader's identity at offset 0x10, or None (v1: code there)."""
    if len(boot) < BOOT_ID_OFFSET + 16:
        return None
    magic, version, features, _, app_max, mailbox = struct.unpack_from("<IBBHII", boot, BOOT_ID_OFFSET)
    if magic != BOOT_ID_MAGIC:
        return None
    return {"version": version, "features": features, "app_max": app_max, "mailbox": mailbox}


def boot_id_problem(bid):
    if bid["version"] < BOOT_VERSION_MIN:
        return f"boot_id version {bid['version']}"
    if bid["mailbox"] != MAILBOX_RAM:
        return f"boot_id mailbox 0x{bid['mailbox']:08X}, not 0x{MAILBOX_RAM:08X}"
    top = BOOT_IMAGE_MAX if bid["features"] & BOOT_FEAT_HIDDEN else FLASH_SIZE - BOOT_SIZE - 0x800
    if not APP_LIMIT_LEGACY <= bid["app_max"] <= top:
        return f"boot_id app_max 0x{bid['app_max']:X}"
    return None


def boot_sector_crc(boot):
    """CRC-32 of the whole 4 KB boot sector, as the app reports it in DP 136 (unused bytes 0xFF)."""
    if len(boot) > BOOT_SIZE:
        raise Refused(f"bootloader is {len(boot)} bytes, max {BOOT_SIZE}")
    return zlib.crc32(bytes(boot) + b"\xff" * (BOOT_SIZE - len(boot))) & 0xFFFFFFFF


def boot_vector_problem(boot):
    """A bootloader the chip can start: SP in RAM, an odd reset vector inside sector 0."""
    if len(boot) < 8:
        return "bootloader shorter than its vector table"
    sp, reset = struct.unpack_from("<II", boot, 0)
    if (sp >> 20) != 0x200 or not (reset & 1) or not BOOT_BASE <= reset < BOOT_BASE + BOOT_SIZE:
        return f"bootloader vectors SP 0x{sp:08X}, reset 0x{reset:08X}: not a bootloader for sector 0"
    return None


def boot_used(boot):
    """Bytes of a boot sector without the 0xFF padding at its end."""
    return len(bytes(boot).rstrip(b"\xff"))


def describe_boot(boot):
    bid = boot_id(boot)
    crc = boot_sector_crc(boot[:BOOT_SIZE])
    if bid is None:
        kind = "v1 or older (no boot_id at 0x08000010)"
    else:
        feats = "+".join(n for b, n in BOOT_FEAT_NAMES if bid["features"] & b) or "none"
        kind = f"version {bid['version']} (features 0x{bid['features']:02X} {feats}, app max {bid['app_max']} bytes)"
    return bid, crc, kind


def check_full(full, erase_settings=False, lines=None, keep_settings=False):
    """The whole-flash image an SWD install writes (section 11, install.sh). Returns the lines
    to show (also collected in `lines` when given, so a refusal can show them); raises Refused."""
    lines = [] if lines is None else lines
    full = bytes(full)
    if len(full) < BOOT_SIZE + APP_SIZE_MIN or full[BOOT_SIZE + APP_HEADER_OFFSET:][:4] != b"APDZ":
        raise Refused("no app header at 0x080010C0 (not a full image?)")
    if len(full) > FLASH_SIZE:
        raise Refused(f"image of {len(full)} bytes: larger than the 20 KB flash")
    boot, rest = full[:BOOT_SIZE], full[BOOT_SIZE:]
    h = header(rest)
    if h.size > len(rest):
        raise Refused(f"app header says {h.size} bytes, the image has {len(rest)} after the bootloader")
    app = rest[:h.size]
    if image_crc(app) != h.crc:
        raise Refused("app CRC mismatch")
    if h.crc == 0:
        raise Refused("app CRC 0: the bootloader uses 0 for 'no image'")
    if any(b != 0xFF for b in rest[h.size:]):
        raise Refused("bytes after the app's end are not 0xFF (not made by mkimage.py --full?)")
    if h.flags & FLAG_SIM:
        raise Refused("SIM bench build: never on a module that goes back on the mains")
    bid, crc, kind = describe_boot(boot)
    lines += [f"image: PY32 {describe(h)}, app {h.size} bytes",
              f"bootloader: {kind}, {boot_used(boot)} of {BOOT_SIZE} bytes used",
              f"boot sector CRC-32: 0x{crc:08X} (DP 136 after the install)"]
    p = boot_vector_problem(boot)
    if p:
        raise Refused(p)
    p = app_problem(app, h)                               # the last gate before a permanent install
    if p:
        raise Refused(f"app: {p}")
    if h.size > BOOT_APP_MAX:
        raise Refused(f"app of {h.size} bytes: its last {h.size - BOOT_APP_MAX} go into the hidden flash "
                      "past the settings ring, which no full image can hold; install it with "
                      "tools/swd/install_hidden.sh (or the bootloader, over the UART)")
    if h.flags & FLAG_MEASURE:
        lines.append("MEASUREMENT build: outputs off, takes no update (back over SWD)")
    if h.flags & FLAG_DIAG:
        lines.append("DIAGNOSTICS build: takes no update (back over SWD)")
    if bid is None:
        raise Refused("the image's bootloader has no boot_id (v1 or older): every module keeps the "
                      "bootloader it gets now for good, and v1 is not installed")
    p = boot_id_problem(bid)
    if p:
        raise Refused(f"bootloader identity: {p}")
    if h.size > bid["app_max"]:
        raise Refused(f"app of {h.size} bytes, the bootloader takes {bid['app_max']}")
    if len(full) > BOOT_SIZE + APP_LIMIT_LEGACY:          # what pyOCD writes is the whole file
        if erase_settings:
            lines.append("ERASE_SETTINGS=1: the settings in sector 4 are erased with the write")
        elif keep_settings:
            # The app's layout keeps its ring in the sector's upper 2 KB (RING_HIGH, required for
            # an app over 12 KB by app_problem above): the image ends below 0x08004800.
            lines.append("settings kept: sector 4 is read first and written back with the image's "
                         "start merged in (the settings ring from 0x08004800 unchanged)")
        else:
            raise Refused(f"the image reaches past 0x08004000 (app {h.size} bytes): pyOCD erases whole "
                          "4 KB sectors, so writing it ERASES THE SETTINGS RING (sector 4). "
                          "ERASE_SETTINGS=1 to accept that")
    return lines


def release_notes_line(boot, full=None):
    if full is not None and bytes(full[:BOOT_SIZE]) != bytes(boot) + b"\xff" * (BOOT_SIZE - len(boot)):
        raise Refused("the full image's boot sector is not boot.bin padded with 0xFF")
    bid, crc, kind = describe_boot(boot)
    return (f"Bootloader: {kind}, {len(boot)} of {BOOT_SIZE} bytes. Boot sector CRC-32 "
            f"(DP 136 after an SWD install of this release's zdms16_py32_full.bin): 0x{crc:08X}.")


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = {a for a in argv[1:] if a.startswith("--")}
    try:
        if (len(args) == 2 and args[0] == "full" and opts <= {"--erase-settings", "--keep-settings"}
                and not {"--erase-settings", "--keep-settings"} <= opts):
            with open(args[1], "rb") as f:
                data = f.read()
            lines = []
            try:
                check_full(data, "--erase-settings" in opts, lines,
                           keep_settings="--keep-settings" in opts)
            finally:
                print("\n".join(lines), flush=True)
        elif len(args) in (2, 3) and args[0] == "notes" and not opts:
            files = []
            for path in args[1:]:
                with open(path, "rb") as f:
                    files.append(f.read())
            print(release_notes_line(*files))
        else:
            sys.exit(__doc__)
    except Refused as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main(sys.argv)
