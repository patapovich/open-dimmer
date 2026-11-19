#!/usr/bin/env python3
"""Check a built app against what docs/BOOTLOADER.md section 9 wants of every build kind:

  - the image header (items 1, 10): flags = APP_FLAG_CONFIRMS plus the bench flag of the kind,
    and RING_HIGH from release M on (build 400: the ring mover, docs/BOOTLOADER.md section 1;
    tools/check_elf.py checks it against the linker layout), nothing else; build = the release
    build FW_BUILD of src/tuya.h (0xFFFE for SIM);
  - the watchdog first (item 2): in main() the watchdog's start (the key 0xCCCC) comes before
    any call and before the clock setup (the HSI calibration read at 0x1FFF0F10 or a call into
    it), so a hang anywhere in the init resets too.

    check_app.py build normal          (build-diag diag, build-measure measure, build-sim sim)
    check_app.py build-t7 normal --build 307

Reads <dir>/zdms16_py32.img and <dir>/zdms16_py32.elf (arm-none-eabi-objdump, or $OBJDUMP).
Used by CI's build job and tools/release/release.sh; the rules themselves are tested on
synthetic input in tests/test_tools.py."""
import os, re, struct, subprocess, sys

APP_MAGIC = 0x5A445041
HDR = 0xC0
CONFIRMS = 0x08
KINDS = {"normal": CONFIRMS, "sim": CONFIRMS | 0x01, "measure": CONFIRMS | 0x02,
         "diag": CONFIRMS | 0x04}
BUILD_SIM = 0xFFFE
RING_HIGH = 0x10
RING_MOVER_BUILD = 400            # release M (2.1.14): the first app with RING_HIGH
IWDG_KEY_START = 0x0000CCCC       # main.c watchdog_start(): IWDG->KR = 0xCCCC
HSI_CALIB_24MHZ = 0x1FFF0F10      # clock_init(): LL_RCC_HSI_SetCalibFreq(LL_RCC_HSICALIBRATION_24MHz)
CLOCK_CALLS = ("clock_init", "SystemCoreClockUpdate", "LL_SetSystemCoreClock", "LL_Init1msTick")


def release_build(tuya_h=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src",
                                      "tuya.h")):
    """FW_BUILD, the literal release build in src/tuya.h."""
    with open(tuya_h) as f:
        m = re.search(r"^#define FW_BUILD\s+(\d+)\s*$", f.read(), re.M)
    if not m:
        raise SystemExit("src/tuya.h: no '#define FW_BUILD <number>'")
    return int(m.group(1))


def check_header(img, kind, build):
    """Errors (a list, empty when fine) of an image's header for a build kind and build."""
    if len(img) < HDR + 16 or struct.unpack_from("<I", img, HDR)[0] != APP_MAGIC:
        return ["no app header at 0xC0"]
    flags, got = img[HDR + 0x0D], struct.unpack_from("<H", img, HDR + 0x0E)[0]
    want = BUILD_SIM if kind == "sim" else build
    errors = []
    wanted = KINDS[kind] | (flags & RING_HIGH if got >= RING_MOVER_BUILD else 0)
    if flags != wanted:
        errors.append("flags 0x%02X, a %s build wants 0x%02X" % (flags, kind, wanted))
    if got != want:
        errors.append("build %d (0x%04X), a %s build wants %d (0x%04X)" % (got, got, kind, want, want))
    return errors


def check_order(disasm):
    """Errors of main()'s disassembly (objdump -d --disassemble=main): the watchdog's start key
    before any call and before the clock setup."""
    words, steps = {}, []        # literal pool: address -> value; instructions in order
    for line in disasm.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+\.word\s+0x([0-9a-f]+)", line)
        if m:
            words[int(m.group(1), 16)] = int(m.group(2), 16)
            continue
        m = re.match(r"\s*([0-9a-f]+):\s+(?:[0-9a-f]{4}\s?){1,2}\s+(\S+)\s*(.*)", line)
        if m:
            steps.append((m.group(2), m.group(3)))
    if not steps:
        return ["no main() in the disassembly"]
    watchdog = clock = first_call = None
    for i, (op, args) in enumerate(steps):
        lit = re.search(r"\[pc, #\d+\]\s*@ \(([0-9a-f]+)", args) if op.startswith("ldr") else None
        value = words.get(int(lit.group(1), 16)) if lit else None
        # "bl <f>", "bl <f+0x12>" (a call into a function's middle, e.g. main's own outlined part
        # after LTO) or "blx r3" (no label: the call itself still counts)
        label = re.search(r"<([^>+]+)(?:\+0x[0-9a-f]+)?>", args) if op in ("bl", "blx") else None
        call = label.group(1) if label else None
        if op in ("bl", "blx") and call != "watchdog_start" and first_call is None:
            first_call = (i, call or args)
        if watchdog is None and (value == IWDG_KEY_START or call == "watchdog_start"):
            watchdog = i
        if clock is None and (value == HSI_CALIB_24MHZ or call in CLOCK_CALLS):
            clock = i
    errors = []
    if watchdog is None:
        return ["main() never starts the watchdog (no IWDG key 0xCCCC, no watchdog_start call)"]
    if clock is not None and clock < watchdog:
        errors.append("main() sets up the clock before it starts the watchdog")
    if first_call is not None and first_call[0] < watchdog:
        errors.append("main() calls %s before it starts the watchdog" % first_call[1])
    return errors


def main(argv):
    args = list(argv)
    build = None
    if "--build" in args:
        i = args.index("--build")
        build = int(args[i + 1], 0)
        del args[i:i + 2]
    if len(args) != 2 or args[1] not in KINDS:
        raise SystemExit("usage: check_app.py <build dir> normal|diag|measure|sim [--build N]")
    bdir, kind = args
    if build is None:
        build = release_build()
    with open(os.path.join(bdir, "zdms16_py32.img"), "rb") as f:
        errors = check_header(f.read(), kind, build)
    objdump = os.environ.get("OBJDUMP", "arm-none-eabi-objdump")
    disasm = subprocess.run([objdump, "-d", "--disassemble=main",
                             os.path.join(bdir, "zdms16_py32.elf")],
                            check=True, capture_output=True, text=True).stdout
    errors += check_order(disasm)
    if errors:
        raise SystemExit("%s (%s): %s" % (bdir, kind, "; ".join(errors)))
    print("%s (%s): flags 0x%02X, build %d, watchdog first in main()"
          % (bdir, kind, KINDS[kind], BUILD_SIM if kind == "sim" else build))


if __name__ == "__main__":
    main(sys.argv[1:])
