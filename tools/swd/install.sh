#!/bin/bash
# One-touch installation of Open Dimmer on a ZDMS16-2's PY32, OFF THE MAINS, powered from the
# probe: start this, THEN press the probe onto CN2 and hold it there until it says DONE.
#
# Works on a stock module (read-protected: the protection is removed, which ERASES the stock
# firmware) and on one that already runs Open Dimmer (just re-flashed). In one contact:
#   1. attach right at power-up (the stock firmware takes the SWD pins a moment later)
#   2. option bytes: read protection off + brown-out reset 2.5 V, written at once
#   3. option reload without a power cycle (FLASH_CR OBL_LAUNCH); if the chip does not come
#      back, it asks you to lift the probe and press it again
#   4. full image (bootloader + app) written and checked, core started
#
# Environment:
#   IMAGE=<zdms16_py32_full.bin>   default: fw/build/zdms16_py32_full.bin; IMAGE=release takes
#                                  the newest release from GitHub
#   PROBE=cmsis-dap|stlink|jlink   OpenOCD interface for the attach (default cmsis-dap = Pico
#                                  debugprobe); pyOCD finds any of them by itself
#   FREQ=<Hz>                      SWD clock for pyOCD (default 4000000; 1000000 if unsure)
#   YES=1                          do not ask before erasing a stock firmware
set -euo pipefail
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../../.." && pwd)"   # ~/projects/zdms16-py32
FW="$ROOT/fw"
PYOCD="${PYOCD:-$ROOT/.venv/bin/pyocd}"
PACK="${PACK:-$ROOT/pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack}"
FREQ="${FREQ:-4000000}"
PROBE="${PROBE:-cmsis-dap}"
IMAGE="${IMAGE:-$FW/build/zdms16_py32_full.bin}"
OB_IMAGE="$FW/tools/swd/ob_bor25.bin"            # RDP 0xAA (none) + BOR on at 2.5 V
TMP=$(mktemp -d)

say()  { printf '\n== %s\n' "$*"; }
beep() { printf '\a'; }
die()  { beep; printf '\n!! %s\n' "$*" >&2; exit 1; }
py()   { "$PYOCD" "$@" -t py32f002ax5 --pack "$PACK" -f "$FREQ" -O connect_mode=attach; }

case "$PROBE" in
    cmsis-dap) OCD_IF=(-f interface/cmsis-dap.cfg -c "transport select swd") ;;
    stlink)    OCD_IF=(-f interface/stlink.cfg -c "transport select hla_swd") ;;
    jlink)     OCD_IF=(-f interface/jlink.cfg -c "transport select swd") ;;
    *)         die "PROBE must be cmsis-dap, stlink or jlink" ;;
esac
[ -x "$PYOCD" ] || die "pyOCD not found at $PYOCD (see docs/INSTALL.md)"
[ -f "$PACK" ]  || die "pack not found: run tools/swd/make_opt_pack.sh"

# --- the image ------------------------------------------------------------------------
if [ "$IMAGE" = "release" ]; then
    IMAGE="$TMP/zdms16_py32_full.bin"
    curl -fsSL -o "$IMAGE" https://github.com/patapovich/zdms16-py32/releases/latest/download/zdms16_py32_full.bin \
        || die "could not download the latest release"
fi
[ -f "$IMAGE" ] || die "image not found: $IMAGE (make, or IMAGE=release)"
python3 - "$IMAGE" <<'PY' || die "not a usable full image"
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
h = 0x1000 + 0xC0                       # app header behind the 4 KB bootloader
assert len(d) > h + 16 and d[h:h + 4] == b"APDZ", "no app header at 0x10C0 (not a full image?)"
size, crc = struct.unpack_from("<II", d, h + 4)
app = bytearray(d[0x1000:0x1000 + size]); app[0xC8:0xCC] = bytes(4)
assert zlib.crc32(app) == crc, "app CRC mismatch"
v, flags = d[h + 12], d[h + 13]
assert not flags & 0x01, "SIM bench build: never on a module that goes back on the mains"
print(f"image: PY32 {v >> 6}.{(v >> 4) & 3}.{v & 15}, app {size} bytes" + (", MEASUREMENT build (outputs off)" if flags & 0x02 else ""))
PY

# --- 1. attach at power-up -------------------------------------------------------------
attach() {   # seconds: keep trying to halt the core; prints OPTR on success
    local end=$(( $(date +%s) + $1 )) out
    while [ "$(date +%s)" -lt "$end" ]; do
        out=$(timeout 5 openocd "${OCD_IF[@]}" -c "adapter speed 1000" \
            -c "swd newdap py32 cpu -expected-id 0" -c "dap create py32.dap -chain-position py32.cpu" \
            -c "target create py32.cpu cortex_m -dap py32.dap" -c "init" -c "halt 100" \
            -c "mww 0x40015804 0x2" -c "echo OPTR=[read_memory 0x40022020 32 1]" -c "shutdown" 2>&1) || true
        if grep -q "OPTR=" <<< "$out"; then
            sed -n 's/.*OPTR=\(0x[0-9a-fA-F]*\).*/\1/p' <<< "$out" | head -1
            return 0
        fi
    done
    return 1
}

say "press the probe onto CN2 now and hold it (module OFF the mains)"
beep
OPTR=$(attach 90) || die "no attach in 90 s: check the wiring (GND, VCC, SWD = PA13, SWC = PA14)"
say "attached, OPTR=$OPTR"
RDP=$(( OPTR & 0xFF ))
BYTE1=$(( (OPTR >> 8) & 0xFF ))

# --- 2./3. option bytes ----------------------------------------------------------------
reload_options() {   # FLASH_CR OBL_LAUNCH: reset with the new option bytes, no power cycle
    py commander -c "write32 0x40022008 0x45670123" -c "write32 0x40022008 0xCDEF89AB" \
        -c "write32 0x4002200C 0x08192A3B" -c "write32 0x4002200C 0x4C5D6E7F" \
        -c "write32 0x40022014 0x08000000" > /dev/null 2>&1 || true   # the reset drops the link
    sleep 1
    local optr
    if optr=$(attach 5); then
        echo "$optr"
        return 0
    fi
    say "lift the probe off CN2 and press it on again"
    beep
    attach 60 || die "no attach after the option reload"
}

if [ "$RDP" -ne $((0xAA)) ] || [ "$BYTE1" -ne $((0xF9)) ]; then
    if [ "$RDP" -ne $((0xAA)) ]; then
        echo "read protection is ON: removing it ERASES the stock firmware (no backup is possible)"
        if [ -z "${YES:-}" ]; then
            read -r -p "erase and install? [y/N] " ok
            [ "$ok" = "y" ] || die "nothing changed"
        fi
    fi
    say "option bytes: protection off, brown-out reset 2.5 V"
    py load --base-address 0x1FFF0E80 "$OB_IMAGE" > "$TMP/ob.log" 2>&1 || { cat "$TMP/ob.log"; die "option byte write failed"; }
    OPTR=$(reload_options)
    say "after reload: OPTR=$OPTR"
    [ $(( OPTR & 0xFF )) -eq $((0xAA)) ] || die "read protection still on (OPTR=$OPTR)"
    [ $(( (OPTR >> 8) & 0xFF )) -eq $((0xF9)) ] || die "BOR not set (OPTR=$OPTR)"
else
    say "already unlocked with BOR: re-flashing"
fi

# --- 4. image --------------------------------------------------------------------------
say "writing the firmware"
py commander -c "write32 0x40015808 0x1000" > /dev/null 2>&1 || true   # freeze the watchdog while halted
py load --format bin --base-address 0x08000000 "$IMAGE" > "$TMP/load.log" 2>&1 || { cat "$TMP/load.log"; die "flash write failed"; }
py commander -c "savemem 0x08000000 $(stat -c %s "$IMAGE") $TMP/readback.bin" > /dev/null 2>&1 || die "read-back failed"
cmp -s "$IMAGE" "$TMP/readback.bin" || die "read-back differs from the image"
py reset > /dev/null 2>&1 || true
beep
say "DONE: firmware written and checked. Lift the probe, close the module; in Zigbee2MQTT mcu_firmware shows 'Open Dimmer' (older converters: mcu_version '2.x.y own')."
