#!/usr/bin/env python3
"""Read-only identity survey of a ZDMS16-2's PY32 over SWD (external probe).

    .venv/bin/python fw/tools/swd/identity.py [--freq HZ] [--json OUT.json]

OFF THE MAINS, powered from the probe (docs/INSTALL.md). Attaches without
halting or resetting the core and reads; the firmware keeps running. Safe on a
running Open Dimmer unit and on a read-protected stock one (there the flash
reads fail, which is reported, not an error).

What does get written is debug state only: the DP power-up request, and
pyOCD's core init sets DHCSR C_DEBUGEN (cleared again at disconnect). The
pack's DebugCoreStart sequence is switched off: it would also write
RCC_APBENR1 DBGEN and DBGMCU CR / APB_FZ1 / APB_FZ2 (DBGMCU_CR = 0x2 and the
freezes cleared), all of which then stay until the next power-on reset.
Nothing reaches the flash, the option bytes or the peripherals the firmware
uses.

What it reads is what the ZTU-hosted SWD master (the planned mcu_flasher) will
check before it writes anything, so its output gives the constants that master
pins, and later the cross-check of the master's own survey against a
known-good probe:

  DP:     DPIDR
  AP 0:   IDR
  core:   CPUID, DHCSR (debug state, read only)
  DBGMCU: IDCODE, CR, APB_FZ1, APB_FZ2
  FLASH:  OPTR, SDKR, WRPR; the stored option area 0x1FFF0E80 (16 B)
  system: UID 0x1FFF0E00 (shown only as a CRC), system ROM CRC
          (0x1FFF0000..0x1FFF0DFF), RCC_ICSCR HSI_FS and its factory flash
          timing words
  flash:  CRC of sector 0 (0x08000000, 4 KB), boot_id (0x08000010), app
          header (0x080010C0); only at RDP level 0

CRCs are zlib CRC-32, as everywhere in this project.
"""
import argparse
import json
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
PACK = os.environ.get("PACK", os.path.join(ROOT, "pack", "Puya.PY32F0xx_DFP.1.1.7-opt.pack"))
TARGET = "py32f002ax5"

# What the test unit showed (the 2026-11-17 dumps; DBGMCU IDCODE and AP IDR
# read with this tool on 2026-11-19), or None where it has never been read.
# Printed next to what is read; a difference is shown, not refused: this tool
# only looks.
KNOWN = {
    "dpidr": 0x0BC11477,
    "ap_idr": 0x04770031,           # ARM AHB-AP of a Cortex-M0+
    "cpuid": 0x410CC601,            # Cortex-M0+ r0p1
    "dbgmcu_idcode": 0x60001000,
    "flash_sdkr": 0x0000001F,       # stored 0x00FF: SDK_STRT 0x1F > SDK_END 0 = no SDK area
    "flash_wrpr": 0x0000FFFF,
    "rom_crc": 0xA52232EA,
    "option_area": "aaf95506ff0000ffffffffffffff0000",   # ob_bor25.bin (Open Dimmer)
}
STOCK_OPTION_AREA = "00f0ff0fff0000ffffffffffffff0000"   # factory: RDP level 1, user byte 0xF0

FLASH = 0x40022000
DBGMCU = 0x40015800
RCC_ICSCR = 0x40021004
TIMING_TABLE = (0x1FFF0F1C, 0x1FFF0F30, 0x1FFF0F44, 0x1FFF0F58, 0x1FFF0F6C)
BOOT_ID_MAGIC = b"ODBL"
APP_MAGIC = b"APDZ"


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def read_block(target, addr: int, size: int):
    """Bytes, or the exception text if the read faults (RDP level 1)."""
    try:
        return bytes(target.read_memory_block8(addr, size))
    except Exception as exc:  # pyOCD raises TransferFaultError and friends
        return f"{type(exc).__name__}: {exc}"


def survey(session) -> dict:
    target = session.target
    out = {}
    out["dpidr"] = target.dp.dpidr.idr
    aps = list(target.aps.values())
    out["ap_idr"] = aps[0].idr if aps else None
    r32 = target.read32
    out["cpuid"] = r32(0xE000ED00)
    out["dhcsr"] = r32(0xE000EDF0)
    out["dbgmcu_idcode"] = r32(DBGMCU + 0x0)
    out["dbgmcu_cr"] = r32(DBGMCU + 0x4)
    out["dbgmcu_apb_fz1"] = r32(DBGMCU + 0x8)
    out["dbgmcu_apb_fz2"] = r32(DBGMCU + 0xC)
    out["flash_optr"] = r32(FLASH + 0x20)
    out["flash_sdkr"] = r32(FLASH + 0x24)
    out["flash_wrpr"] = r32(FLASH + 0x2C)
    out["option_area"] = read_block(target, 0x1FFF0E80, 16)
    uid = read_block(target, 0x1FFF0E00, 16)
    out["uid_crc"] = crc32(uid) if isinstance(uid, bytes) else uid
    rom = read_block(target, 0x1FFF0000, 0xE00)
    out["rom_crc"] = crc32(rom) if isinstance(rom, bytes) else rom
    icscr = r32(RCC_ICSCR)
    fs = (icscr >> 13) & 0x7
    out["rcc_icscr"] = icscr
    out["hsi_fs"] = fs
    if fs < len(TIMING_TABLE):
        out["timing_words"] = [r32(TIMING_TABLE[fs] + 4 * i) for i in range(5)]
    rdp0 = (out["flash_optr"] & 0xFF) == 0xAA
    out["rdp"] = "level 0" if rdp0 else ("level 2" if (out["flash_optr"] & 0xFF) == 0xCC else "level 1")
    sector0 = read_block(target, 0x08000000, 4096)
    if isinstance(sector0, bytes):
        out["sector0_crc"] = crc32(sector0)
        bid = sector0[0x10:0x20]
        if bid[:4] == BOOT_ID_MAGIC:
            _, ver, feat, _res, app_max, mailbox = struct.unpack("<4sBBHII", bid)
            out["boot_id"] = {"version": ver, "features": feat, "app_max": app_max, "mailbox": mailbox}
        else:
            out["boot_id"] = None
        out["vectors"] = list(struct.unpack("<II", sector0[:8]))
    else:
        out["sector0_crc"] = sector0
    hdr = read_block(target, 0x080010C0, 16)
    if isinstance(hdr, bytes) and hdr[:4] == APP_MAGIC:
        _, size, crc, ver, flags, build = struct.unpack("<4sIIBBH", hdr)
        out["app_header"] = {"size": size, "crc": crc, "version": ver, "flags": flags, "build": build}
    elif isinstance(hdr, bytes):
        out["app_header"] = None
    else:
        out["app_header"] = hdr
    return out


def show(res: dict) -> None:
    def fmt(v):
        if isinstance(v, bytes):
            return v.hex()
        if isinstance(v, int):
            return f"0x{v:08X}"
        return str(v)

    for key, val in res.items():
        line = f"{key:16s} {fmt(val)}"
        want = KNOWN.get(key)
        got = val.hex() if isinstance(val, bytes) else val
        if key == "option_area" and got == STOCK_OPTION_AREA:
            line += "   (= factory option bytes, RDP level 1)"
        elif want is not None:
            line += "   OK" if got == want else f"   DIFFERS from {fmt(want)}"
        elif key in KNOWN:
            line += "   (new: pin this value)"
        print(line)
    dhcsr = res.get("dhcsr", 0)
    if isinstance(dhcsr, int):
        state = ["debug on" if dhcsr & 1 else "debug off", "halted" if dhcsr & (1 << 17) else "running"]
        if dhcsr & (1 << 19):
            state.append("LOCKUP")
        print(f"{'core':16s} {', '.join(state)}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--freq", type=int, default=400000, help="SWD clock in Hz (default 400 kHz)")
    ap.add_argument("--json", help="also write the values to this file")
    args = ap.parse_args()
    from pyocd.core.helpers import ConnectHelper  # the project .venv (pyOCD 0.46)

    options = {"connect_mode": "attach", "frequency": args.freq, "pack": PACK,
               "resume_on_disconnect": True,
               "pack.debug_sequences.disabled_sequences": "DebugCoreStart"}
    with ConnectHelper.session_with_chosen_probe(target_override=TARGET, options=options) as session:
        res = survey(session)
    show(res)
    if args.json:
        with open(args.json, "w") as f:
            json.dump({k: (v.hex() if isinstance(v, bytes) else v) for k, v in res.items()}, f, indent=1)
        print(f"written {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
