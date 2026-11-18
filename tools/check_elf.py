#!/usr/bin/env python3
"""ELF checks of the CI build job (docs/BOOTLOADER.md section 11), from arm-none-eabi-nm:

    check_elf.py boot build-boot/boot.elf build-boot/boot.bin
        k_boot_id at 0x08000010, 16 bytes; the flash write guard _flash_write_start/_end exactly
        0x08001000/0x08004800 (the app area); boot.bin at most 4096 bytes (a warning above 3584);
        _ebss + 512 bytes of stack at most 0x20000BE0 (the mailbox stays outside)
    check_elf.py app build/zdms16_py32.elf build/zdms16_py32.img
        RING_HIGH set exactly when _settings_start is 0x08004800 (and the ring is at 0x08004000
        otherwise); a flash write guard, when the app has one, is its settings region
Warnings and errors are printed as GitHub annotations; the exit code is 1 on any error.
"""
import os
import subprocess
import sys

sys.dont_write_bytecode = True                         # no __pycache__ in tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fwimage as fw                                   # noqa: E402

BOOT_WARN_ABOVE = 3584          # 512 bytes of headroom left in the 4 KB sector
BOOT_STACK = 512
NM = os.environ.get("NM", "arm-none-eabi-nm")


def symbols(nm_output):
    """{name: (address, size or None)} from `nm -S` output (local symbols included)."""
    syms = {}
    for line in nm_output.splitlines():
        p = line.split()
        if len(p) == 4:
            syms[p[3]] = (int(p[0], 16), int(p[1], 16))
        elif len(p) == 3:
            syms.setdefault(p[2], (int(p[0], 16), None))
    return syms


def check_boot(syms, bin_size):
    errors, warnings, info = [], [], []
    bid = syms.get("k_boot_id")
    if bid is None:
        errors.append("boot.elf has no k_boot_id (boot_id_t)")
    elif bid != (fw.BOOT_BASE + fw.BOOT_ID_OFFSET, 16):
        errors.append(f"k_boot_id at 0x{bid[0]:08X}, {bid[1]} bytes: must be at 0x08000010, 16 bytes")
    for name, want in (("_flash_write_start", fw.APP_BASE), ("_flash_write_end", fw.APP_BASE + fw.BOOT_APP_MAX)):
        s = syms.get(name)
        if s is None:
            errors.append(f"boot.elf has no {name} (the flash driver's write guard)")
        elif s[0] != want:
            errors.append(f"{name} = 0x{s[0]:08X}, must be 0x{want:08X}")
    if bin_size > fw.BOOT_SIZE:
        errors.append(f"boot.bin is {bin_size} bytes, more than the {fw.BOOT_SIZE}-byte sector")
    elif bin_size > BOOT_WARN_ABOVE:
        warnings.append(f"boot.bin is {bin_size} of {fw.BOOT_SIZE} bytes ({fw.BOOT_SIZE - bin_size} free, "
                        f"warning above {BOOT_WARN_ABOVE})")
    ebss = syms.get("_ebss")
    if ebss is None:
        errors.append("boot.elf has no _ebss")
    elif ebss[0] + BOOT_STACK > fw.MAILBOX_RAM:
        errors.append(f"_ebss 0x{ebss[0]:08X} + {BOOT_STACK} bytes of stack reach past the mailbox 0x{fw.MAILBOX_RAM:08X}")
    info.append(f"bootloader: {bin_size} of {fw.BOOT_SIZE} bytes, RAM to 0x{ebss[0] if ebss else 0:08X} + stack")
    return errors, warnings, info


def check_app(syms, img):
    errors, warnings, info = [], [], []
    h = fw.header(img)
    ring = syms.get("_settings_start")
    if ring is None:
        errors.append("the app ELF has no _settings_start")
        return errors, warnings, info
    high = bool(h.flags & fw.FLAG_RING_HIGH)
    want = fw.APP_BASE + fw.BOOT_APP_MAX if high else fw.APP_BASE + fw.APP_LIMIT_LEGACY
    if ring[0] != want:
        errors.append(f"_settings_start 0x{ring[0]:08X} with RING_HIGH {'set' if high else 'clear'}: "
                      f"must be 0x{want:08X} (one Makefile switch sets both)")
    ws, we, se = syms.get("_flash_write_start"), syms.get("_flash_write_end"), syms.get("_settings_end")
    if ws is None and we is None:
        info.append("the app has no flash write guard symbols (_flash_write_start/_end)")
    elif ws is None or we is None or se is None or ws[0] != ring[0] or we[0] != se[0]:
        errors.append("the app's flash write guard _flash_write_start/_end is not its settings region "
                      "_settings_start/_end")
    info.append(f"app: {len(img)} bytes, settings ring at 0x{ring[0]:08X}, flags 0x{h.flags:02X}")
    return errors, warnings, info


def main(argv):
    if len(argv) != 4 or argv[1] not in ("boot", "app"):
        sys.exit(__doc__)
    out = subprocess.run([NM, "-S", argv[2]], capture_output=True, text=True, check=True).stdout
    syms = symbols(out)
    with open(argv[3], "rb") as f:
        data = f.read()
    errors, warnings, info = check_boot(syms, len(data)) if argv[1] == "boot" else check_app(syms, data)
    for line in info:
        print(line)
    for w in warnings:
        print(f"::warning title=Flash headroom::{w}")
    for e in errors:
        print(f"::error::{e}")
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main(sys.argv)
