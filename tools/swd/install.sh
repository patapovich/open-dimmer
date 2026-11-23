#!/bin/bash
# One-touch installation of Open Dimmer on a ZDMS16-2's PY32, OFF THE MAINS, powered from the
# probe: start this, THEN press the probe onto CN2 and hold it there until it says DONE.
#
# Works on a stock module (read-protected: the protection is removed, which ERASES the stock
# firmware) and on one that already runs Open Dimmer (just re-flashed). In one contact:
#   1. attach right at power-up (the stock firmware takes the SWD pins a moment later)
#   2. option bytes: read protection off + brown-out reset 2.5 V (+ sector 0 write protection
#      off, if it was on), written at once
#   3. option reload without a power cycle (FLASH_CR OBL_LAUNCH); if the chip does not come
#      back, it asks you to lift the probe and press it again
#   4. full image (bootloader + app) written and checked, core started
#   5. with WRP=1: sector 0 (the bootloader) write-protected, reloaded and checked
#
# The bootloader written here stays on the module for good (docs/BOOTLOADER.md): the image's
# bootloader version (boot_id) and its boot sector CRC (what the app reports as DP 136) are
# shown first. A debugger reset keeps the RAM, so the boot word of the mailbox (0x20000BF8,
# 0x20000BFC) is written 0 before every reset this issues: the new firmware starts with fresh
# tries. On a read-protected (stock) chip only from the image load on: the stock firmware has no
# mailbox, removing the protection erases the flash, and a pyOCD session more between the attach
# and the option reload only gives the stock firmware time to take the SWD pins.
#
# Environment:
#   IMAGE=<zdms16_py32_full.bin>   default: fw/build/zdms16_py32_full.bin; IMAGE=release takes
#                                  the newest release from GitHub, IMAGE=release:vX.Y.Z that
#                                  one; both are checked against the release's SHA256SUMS,
#                                  bootloader region included (boot.bin + app image)
#   ERASE_SETTINGS=1               an image over 12 KB erases the settings in sector 4 (pyOCD
#                                  erases whole 4 KB sectors). Without it such an image keeps
#                                  them: sector 4 is read first and written back with the
#                                  image's start merged in (as an update over the UART would)
#   WRP=1                          write-protect sector 0 (the bootloader) after the write
#                                  (docs/BOOTLOADER.md section 1: not set in phase 3; try it
#                                  on the spare first). A re-flash clears it first in any case
#   PROBE=cmsis-dap|stlink|jlink   OpenOCD interface for the attach (default cmsis-dap = Pico
#                                  debugprobe); pyOCD finds any of them by itself
#   FREQ=<Hz>                      SWD clock for pyOCD (default 4000000; 1000000 if unsure)
#   YES=1                          do not ask before erasing a stock firmware
set -euo pipefail
FW="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"   # this repo (the clone, "fw")
ROOT="$(dirname "$FW")"                                       # its work directory: .venv, pack/
PYOCD="${PYOCD:-$ROOT/.venv/bin/pyocd}"
PACK="${PACK:-$ROOT/pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack}"
FREQ="${FREQ:-4000000}"
PROBE="${PROBE:-cmsis-dap}"
IMAGE="${IMAGE:-$FW/build/zdms16_py32_full.bin}"
REPO_URL="${REPO_URL:-https://github.com/patapovich/open-dimmer}"
OB_IMAGE="$FW/tools/swd/ob_bor25.bin"            # RDP 0xAA (none) + BOR on at 2.5 V, WRPR 0xFFFF
STOCK=""                                          # 1 while a read-protected (stock) firmware is on the chip
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

say()  { printf '\n== %s\n' "$*"; }
beep() { printf '\a'; }
die()  { beep; printf '\n!! %s\n' "$*" >&2; exit 1; }
py()   { "$PYOCD" "$@" -t py32f002ax5 --pack "$PACK" -f "$FREQ" -O connect_mode=attach; }

# The boot word pair of the mailbox, written 0 (docs/BOOTLOADER.md section 3). The watchdog is
# frozen while the core is halted (DBGMCU APBFZ1 DBG_IWDG_STOP): the v2 bootloader starts it at
# every boot.
BOOT_WORD_ZERO=(-c "write32 0x20000BF8 0" -c "write32 0x20000BFC 0")
IWDG_FREEZE=(-c "write32 0x40015808 0x1000")
zero_boot_word() {   # best effort, before a reset the chip does by itself (load, option reload).
                     # A core the attach halted stays halted until that reset (pyOCD's commander
                     # does not resume by default; said here so that no version or config does).
    [ -z "$STOCK" ] || return 0                    # stock firmware: see the top
    py commander -O resume_on_disconnect=false "${IWDG_FREEZE[@]}" "${BOOT_WORD_ZERO[@]}" > /dev/null 2>&1 || true
}
reset_clean() {      # halt, boot word 0 (read back), reset: the last step
    local out
    if ! out=$(py commander "${IWDG_FREEZE[@]}" -c halt "${BOOT_WORD_ZERO[@]}" -c "read32 0x20000BF8 8" -c reset 2>&1); then
        py reset > /dev/null 2>&1 || true            # start the firmware in any case
        echo "warning: the boot word could not be cleared before the reset; the next power-up clears it" >&2
    elif ! grep -qiE '^20000bf8: +0+ +0+( |$)' <<< "$out"; then
        echo "warning: the boot word did not read back 0 before the reset; the next power-up clears it" >&2
    fi
}

case "$PROBE" in
    cmsis-dap) OCD_IF=(-f interface/cmsis-dap.cfg -c "transport select swd") ;;
    stlink)    OCD_IF=(-f interface/stlink.cfg -c "transport select hla_swd") ;;
    jlink)     OCD_IF=(-f interface/jlink.cfg -c "transport select swd") ;;
    *)         die "PROBE must be cmsis-dap, stlink or jlink" ;;
esac
for v in ERASE_SETTINGS WRP; do   # "0" must not mean yes
    case "${!v:-}" in "" | 1) ;; *) die "$v must be 1 or unset" ;; esac
done
[ -x "$PYOCD" ] || die "pyOCD not found at $PYOCD (see docs/INSTALL-PROGRAMMER.md)"
[ -f "$PACK" ]  || die "pack not found: run tools/swd/make_opt_pack.sh"
[ "$(od -An -tx1 -v "$OB_IMAGE" | tr -d ' \n')" = "aaf95506ff0000ffffffffffffff0000" ] ||
    die "$OB_IMAGE is not the expected option bytes (RDP off, BOR 2.5 V, no write protection)"

# --- the image ------------------------------------------------------------------------
if [ "$IMAGE" = "release" ] || [ "${IMAGE#release:}" != "$IMAGE" ]; then
    TAG=${IMAGE#release}; TAG=${TAG#:}
    if [ -z "$TAG" ]; then
        url=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$REPO_URL/releases/latest") ||
            die "could not find the latest release"
        TAG=${url##*/}
    fi
    [[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "not a release tag: '$TAG'"
    say "release $TAG"
    REL="$TMP/release"
    mkdir -p "$REL"
    for f in zdms16_py32_full.bin boot.bin zdms16_py32.img SHA256SUMS; do
        curl -fsSL -o "$REL/$f" "$REPO_URL/releases/download/$TAG/$f" && continue
        # From 2.2.8 the app uses the hidden flash (bootloader v3, docs/BOOTLOADER.md section 18):
        # its part past 14 KB lies beyond the settings ring, so the release has no full image.
        [ "$f" = zdms16_py32_full.bin ] &&
            curl -fsSL -o "$REL/zdms16_py32.img" "$REPO_URL/releases/download/$TAG/zdms16_py32.img" &&
            die "$TAG has no full image: its app uses the hidden flash (bootloader v3), which this script
cannot write yet. Install IMAGE=release:v2.2.6 for now: that module keeps bootloader v2 and so
stays on 2.2.6 until this script writes v3 (run it again then; the settings stay)."
        die "could not download $f of $TAG"
    done
    # Lists up to v2.1.11 name build/ paths: compare by file name. Every file must be listed.
    sed 's|  .*/|  |; s| \*.*/| *|' "$REL/SHA256SUMS" > "$REL/sums"
    for f in zdms16_py32_full.bin boot.bin zdms16_py32.img; do
        grep -qE "^[0-9a-f]{64}  \*?$f\$" "$REL/sums" || die "SHA256SUMS of $TAG does not list $f"
    done
    (cd "$REL" && grep -E '  \*?(zdms16_py32_full\.bin|boot\.bin|zdms16_py32\.img)$' sums | sha256sum -c --quiet -) ||
        die "the files of $TAG do not match its SHA256SUMS"
    # The full image is exactly boot.bin padded with 0xFF to 4 KB, then the app image.
    python3 - "$REL" <<'PY' || die "the full image of $TAG is not its boot.bin + app image"
import sys
d = sys.argv[1]
full, boot, app = (open(f"{d}/{n}", "rb").read() for n in ("zdms16_py32_full.bin", "boot.bin", "zdms16_py32.img"))
if len(boot) > 4096 or full != boot + b"\xff" * (4096 - len(boot)) + app:
    sys.exit(1)
PY
    IMAGE="$REL/zdms16_py32_full.bin"
    say "SHA256SUMS of $TAG: full image, bootloader and app image OK"
fi
[ -f "$IMAGE" ] || die "image not found: $IMAGE (make, or IMAGE=release)"
python3 "$FW/tools/fwimage.py" full "$IMAGE" \
    $([ -n "${ERASE_SETTINGS:-}" ] && echo --erase-settings || echo --keep-settings) ||
    die "not a usable full image"
BOOT_CRC=$(python3 -c "import sys, zlib; print('0x%08X' % zlib.crc32(open(sys.argv[1], 'rb').read()[:4096]))" "$IMAGE")
WRPR_ON=0xFFFE   # sector 0 protected (WRPR bit 0 = 0)

# --- 1. attach at power-up -------------------------------------------------------------
attach() {   # seconds: keep trying to halt the core; prints "OPTR WRPR" on success
    local end=$(( $(date +%s) + $1 )) out optr wrpr
    while [ "$(date +%s)" -lt "$end" ]; do
        out=$(timeout 5 openocd "${OCD_IF[@]}" -c "adapter speed 1000" \
            -c "swd newdap py32 cpu -expected-id 0" -c "dap create py32.dap -chain-position py32.cpu" \
            -c "target create py32.cpu cortex_m -dap py32.dap" -c "init" -c "halt 100" \
            -c "mww 0x40015808 0x1000" -c "mww 0x40015804 0x2" -c "echo OPTR=[read_memory 0x40022020 32 1]" \
            -c "echo WRPR=[read_memory 0x4002202C 32 1]" -c "shutdown" 2>&1) || true
        if grep -q "OPTR=" <<< "$out"; then
            optr=$(sed -n 's/.*OPTR=\(0x[0-9a-fA-F]*\).*/\1/p' <<< "$out" | head -1)
            wrpr=$(sed -n 's/.*WRPR=\(0x[0-9a-fA-F]*\).*/\1/p' <<< "$out" | head -1)
            echo "$optr ${wrpr:-0xFFFF}"
            return 0
        fi
    done
    return 1
}

say "press the probe onto CN2 now and hold it (module OFF the mains)"
beep
regs=$(attach 90) || die "no attach in 90 s: check the wiring (GND, VCC, SWD = PA13, SWC = PA14)"
read -r OPTR WRPR <<< "$regs"
say "attached, OPTR=$OPTR WRPR=$WRPR"
RDP=$(( OPTR & 0xFF ))
BYTE1=$(( (OPTR >> 8) & 0xFF ))
# Read-protected: the stock firmware runs. It takes the SWD pins about 1.5 s after it starts
# (docs/stock-firmware-analysis.md), so from here to the option reload only the sessions that
# must be (option byte write, OBL_LAUNCH), and no boot word clear (see the top).
[ "$RDP" -eq $((0xAA)) ] || STOCK=1

# --- 2./3. option bytes ----------------------------------------------------------------
reload_options() {   # FLASH_CR OBL_LAUNCH: reset with the new option bytes, no power cycle.
                     # Prints "OPTR WRPR" after it; everything for the user goes to stderr.
    zero_boot_word                                 # not on a stock chip: it runs again since the
                                                   # option write's reset, and the OBL must come first
    py commander -c "write32 0x40022008 0x45670123" -c "write32 0x40022008 0xCDEF89AB" \
        -c "write32 0x4002200C 0x08192A3B" -c "write32 0x4002200C 0x4C5D6E7F" \
        -c "write32 0x40022014 0x08000000" > /dev/null 2>&1 || true   # the reset drops the link
    sleep 1
    local regs
    if regs=$(attach 5); then
        echo "$regs"
        return 0
    fi
    say "lift the probe off CN2 and press it on again" >&2
    beep >&2
    attach 60 || die "no attach after the option reload"
}

if [ "$RDP" -ne $((0xAA)) ] || [ "$BYTE1" -ne $((0xF9)) ] || [ $(( WRPR & 0xFFFF )) -ne $((0xFFFF)) ]; then
    if [ "$RDP" -ne $((0xAA)) ]; then
        echo "read protection is ON: removing it ERASES the stock firmware (no backup is possible)"
        if [ -z "${YES:-}" ]; then
            read -r -p "erase and install? [y/N] " ok
            [ "$ok" = "y" ] || die "nothing changed"
        fi
    fi
    [ $(( WRPR & 0xFFFF )) -eq $((0xFFFF)) ] || echo "sector write protection is on (WRPR=$WRPR): cleared first"
    say "option bytes: protection off, brown-out reset 2.5 V, no write protection"
    zero_boot_word                                 # pyOCD resets after the write (not on a stock chip)
    py load --base-address 0x1FFF0E80 "$OB_IMAGE" > "$TMP/ob.log" 2>&1 || { cat "$TMP/ob.log"; die "option byte write failed"; }
    regs=$(reload_options) || die "no attach after the option reload"
    read -r OPTR WRPR <<< "$regs"
    say "after reload: OPTR=$OPTR WRPR=$WRPR"
    [ $(( OPTR & 0xFF )) -eq $((0xAA)) ] || die "read protection still on (OPTR=$OPTR)"
    [ $(( (OPTR >> 8) & 0xFF )) -eq $((0xF9)) ] || die "BOR not set (OPTR=$OPTR)"
    [ $(( WRPR & 0xFFFF )) -eq $((0xFFFF)) ] || die "write protection still on (WRPR=$WRPR)"
    STOCK=""                                       # the flash is erased: no stock firmware from here on
else
    say "already unlocked with BOR, no write protection: re-flashing"
fi

# --- 4. image --------------------------------------------------------------------------
LOAD="$IMAGE"
if [ "$(stat -c %s "$IMAGE")" -gt 16384 ] && [ -z "${ERASE_SETTINGS:-}" ]; then
    # The image reaches into sector 4, which pyOCD erases whole: read it first, write it back with
    # the image's bytes merged in at its start (they end below the ring at 0x08004800).
    say "keeping the settings: sector 4 read first, written back with the image"
    py commander -c "savemem 0x08004000 4096 $TMP/sector4.bin" > /dev/null 2>&1 || die "could not read sector 4 (the settings) before the write"
    [ "$(stat -c %s "$TMP/sector4.bin" 2>/dev/null || echo 0)" -eq 4096 ] || die "sector 4 read short"
    python3 - "$IMAGE" "$TMP/sector4.bin" "$TMP/merged.bin" <<'PY' || die "could not merge the image into sector 4"
import sys
img = open(sys.argv[1], "rb").read()
s4 = open(sys.argv[2], "rb").read()
cut = len(img) - 16384              # the image's bytes in sector 4: at most 2 KB (an app of 14 KB)
if not 0 < cut <= 0x800 or len(s4) != 4096:
    sys.exit(f"image end {cut} bytes into sector 4: past the ring at 0x08004800")
open(sys.argv[3], "wb").write(img + s4[cut:])
PY
    LOAD="$TMP/merged.bin"
fi
say "writing the firmware"
zero_boot_word                                     # pyOCD resets after the write
py load --format bin --base-address 0x08000000 "$LOAD" > "$TMP/load.log" 2>&1 || { cat "$TMP/load.log"; die "flash write failed"; }
py commander -c "savemem 0x08000000 $(stat -c %s "$LOAD") $TMP/readback.bin" > /dev/null 2>&1 || die "read-back failed"
cmp -s "$LOAD" "$TMP/readback.bin" || die "read-back differs from the image"

# --- 5. sector 0 write protection (WRP=1) -----------------------------------------------
if [ -n "${WRP:-}" ]; then
    say "option bytes: write protection of sector 0 (the bootloader)"
    python3 - "$OB_IMAGE" "$TMP/ob_wrp0.bin" <<'PY'
import sys
ob = bytearray(open(sys.argv[1], "rb").read())
ob[12:16] = bytes([0xFE, 0xFF, 0x01, 0x00])        # WRPR 0xFFFE + complement: sector 0 only
open(sys.argv[2], "wb").write(ob)
PY
    zero_boot_word
    py load --base-address 0x1FFF0E80 "$TMP/ob_wrp0.bin" > "$TMP/ob.log" 2>&1 || { cat "$TMP/ob.log"; die "option byte write (WRP) failed"; }
    regs=$(reload_options) || die "no attach after the option reload"
    read -r OPTR WRPR <<< "$regs"
    say "after reload: OPTR=$OPTR WRPR=$WRPR"
    [ $(( OPTR & 0xFF )) -eq $((0xAA)) ] && [ $(( (OPTR >> 8) & 0xFF )) -eq $((0xF9)) ] ||
        die "option bytes changed by the WRP write (OPTR=$OPTR)"
    [ $(( WRPR & 0xFFFF )) -eq $((WRPR_ON)) ] || die "sector 0 is not write-protected (WRPR=$WRPR)"
fi
reset_clean
beep
say "DONE: firmware written and checked; boot sector CRC $BOOT_CRC (DP 136)$([ -n "${WRP:-}" ] && echo ', sector 0 write-protected'). Lift the probe, close the module; in Zigbee2MQTT mcu_firmware shows 'Open Dimmer' (older converters: mcu_version '2.x.y own')."
