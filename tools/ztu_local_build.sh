#!/bin/bash
# Local build of the ZDMS16-2 Zigbee firmware (the ZTU fork's MODULE_AVATTO_ZDMS16_2 router image and
# its OTA file) for quick rounds on the spare, without the CI chain (release, py32-bump, module
# build: ~20 min). The version is made as the fork's CI makes it (board.mk): swBuildId
# "<VERSION>-<short hash>", file version 0x<maj><min><patch><0x30 + depth / 256><depth % 256>,
# depth = commits since the fork's main, + 1 for the CI's fork-ci commit; so a local build of a
# commit the CI also built has the same file version, and a build of a newer commit a higher one
# (the module takes only a different version). Only committed work is built (the hash must say
# what is inside). The tree's tracked files are not touched: the build goes to <tree>/build/
# (ignored), the OTA file is copied to $OUT.
# Releases to the house still go through the CI chain (traceable builds).
#
# Usage: tools/ztu_local_build.sh [<fork tree>]          (default ~/projects/tzs-diag)
#   then: IMAGE=<printed .zigbee path> HA_HELPERS=~/ha-claude tools/ha/deploy_ota.sh <spare ieee>
# Environment (optional):
#   ZTU_REMOTE  the fork's remote (default patapovich): its main is where the depth is counted from
#   TELINK_SDK  Telink Zigbee SDK 3.7.2.0 (default ~/tools/telink_zigbee_sdk-3.7.2.0)
#   TELINK_TC   directory holding the tc32 toolchain dir (default ~/tools/tc32)
#   OUT         where the OTA file is copied (default ~/projects/zdms16-py32/build-ztu)
#   PYTHON      Python with PyYAML (default ~/projects/tuya-zigbee-switch/.venv/bin/python)
set -euo pipefail
TREE=$(readlink -f "${1:-$HOME/projects/tzs-diag}")
ZTU_REMOTE=${ZTU_REMOTE:-patapovich}
TELINK_SDK=${TELINK_SDK:-$HOME/tools/telink_zigbee_sdk-3.7.2.0}
TELINK_TC=${TELINK_TC:-$HOME/tools/tc32}
OUT=${OUT:-$HOME/projects/zdms16-py32/build-ztu}
PYTHON=${PYTHON:-$HOME/projects/tuya-zigbee-switch/.venv/bin/python}
BOARD=MODULE_AVATTO_ZDMS16_2
die() { echo "ztu_local_build: $*" >&2; exit 1; }

[ -f "$TREE/device_db.yaml" ] && [ -d "$TREE/src/telink" ] || die "$TREE is not the ZTU fork's tree"
[ -z "$(git -C "$TREE" status --porcelain --untracked-files=no)" ] ||
    die "$TREE has uncommitted changes: commit them first (the build id is the commit's hash)"
[ -d "$TELINK_SDK" ] || die "Telink SDK not found: $TELINK_SDK"
[ -x "$TELINK_TC/tc32/bin/tc32-elf-gcc" ] || die "tc32 toolchain not found: $TELINK_TC/tc32/bin"

# The tree's telink_tools/ (ignored by git) points at the local SDK and toolchain.
mkdir -p "$TREE/telink_tools/toolchain"
[ -e "$TREE/telink_tools/sdk" ] || ln -s "$TELINK_SDK" "$TREE/telink_tools/sdk"
[ -e "$TREE/telink_tools/toolchain/tc32" ] || ln -s "$TELINK_TC/tc32" "$TREE/telink_tools/toolchain/tc32"

# The version as board.mk makes it in the fork's CI (there origin = the fork).
git -C "$TREE" fetch -q "$ZTU_REMOTE" main || die "fetching $ZTU_REMOTE/main failed"
V=$(cat "$TREE/VERSION")
IFS=. read -r MAJ MIN PAT <<< "$V"
HASH=$(git -C "$TREE" rev-parse --short=8 HEAD)
# CI builds home-dimmer_build = this branch + the fork-ci commit: one deeper than the branch itself.
DEPTH=$(( $(git -C "$TREE" rev-list --count "$ZTU_REMOTE/main..HEAD") + 1 ))
VERSION_STR=$(echo "$V-$HASH" | cut -c1-16)
# board.mk: a depth over 255 carries into the third byte (STACK_RELEASE 0x30 + depth / 256).
FILE_VERSION=$(printf '0x%s%s%02d%02X%02X' "$MAJ" "$MIN" "$PAT" $((48 + DEPTH / 256)) $((DEPTH % 256)))

eval "$("$PYTHON" -I - "$TREE/device_db.yaml" "$BOARD" <<'PY'
import shlex, sys, yaml
b = yaml.safe_load(open(sys.argv[1]))[sys.argv[2]]
dp = "".join(a["dp"] + a["type"] + a["attr"] + ";" for a in (b.get("dp_attributes") or []))
sec = b.get("secondary_mcu_image") or ""
for k, v in (("DEVICE_TYPE", b["device_type"]), ("CONFIG_STR", b["config_str"]), ("DP_CONFIG_STR", dp),
             ("SECONDARY_MCU_IMAGE", "true" if sec is True else str(sec)),
             ("DIMMER_ONLY", "true" if b.get("dimmer_only") else ""),
             ("IMAGE_TYPE", str(b.get("firmware_image_type")))):
    print(f"{k}={shlex.quote(v)}")
PY
)"

cd "$TREE"
mkdir -p build/ota "$OUT"
ARGS=(VERSION_STR="$VERSION_STR" FILE_VERSION="$FILE_VERSION" NVM_MIGRATIONS_VERSION="$(cat NVM_MIGRATIONS_VERSION)"
      DEVICE_TYPE="$DEVICE_TYPE" "CONFIG_STR=$CONFIG_STR" "DP_CONFIG_STR=$DP_CONFIG_STR"
      "SECONDARY_MCU_IMAGE=$SECONDARY_MCU_IMAGE" "DIMMER_ONLY=$DIMMER_ONLY" IMAGE_TYPE="$IMAGE_TYPE"
      BIN_FILE=../../build/telink/bin/tlc_switch.bin)
echo "building $BOARD $VERSION_STR (file version $FILE_VERSION) from $TREE"
make -C src/telink clean > /dev/null
make -C src/telink build "${ARGS[@]}" -j"$(nproc)" > build/local-build.log 2>&1 ||
    { tail -20 build/local-build.log; die "build failed (log: $TREE/build/local-build.log)"; }
F="$OUT/tlc_switch-$VERSION_STR.zigbee"
make -C src/telink ota "${ARGS[@]}" OTA_IMAGE_TYPE="$IMAGE_TYPE" OTA_FILE="$TREE/build/ota/local.zigbee" \
    >> build/local-build.log 2>&1 || { tail -20 build/local-build.log; die "OTA file failed"; }
cp build/ota/local.zigbee "$F"
SIZE=$(stat -c %s "$F")
[ "$SIZE" -lt $((0x34000)) ] || die "OTA file of $SIZE bytes: over the 0x34000 limit"
echo "OTA file: $F ($SIZE bytes, $(( 0x34000 - SIZE )) below the limit)"
echo "deploy:   IMAGE=$F HA_HELPERS=~/ha-claude ~/projects/zdms16-py32/fw/tools/ha/deploy_ota.sh <ieee>"
