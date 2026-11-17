#!/bin/bash
# Release the PY32 firmware end to end:
#   checks (clean tree, host tests, build) -> tag vX.Y.Z from FW_VERSION -> CI builds and
#   publishes the release -> py32-bump in patapovich/tuya-zigbee-switch carries it into
#   home-dimmer -> the module firmware (OTA image) is built on home-dimmer_build.
# Deploying to modules is a separate, local step: tools/ha/deploy_ota.sh <ieee>...
# Usage: tools/release/release.sh            (bump FW_VERSION in src/tuya.h first)
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.."
PY32=patapovich/zdms16-py32
ZTU=patapovich/tuya-zigbee-switch

[ -z "$(git status --porcelain)" ] || { echo "uncommitted changes"; exit 1; }
make test > /dev/null
make > /dev/null
TAG=$(python3 -c "d=open('build/zdms16_py32.img','rb').read(); v=d[0xCC]; print(f'v{v>>6}.{(v>>4)&3}.{v&15}')")
if git rev-parse -q --verify "refs/tags/$TAG" > /dev/null; then
    echo "$TAG already released: bump FW_VERSION in src/tuya.h"; exit 1
fi

latest_run() {   # repo, extra gh run list args...: id of the newest run
    local repo=$1; shift
    gh run list --repo "$repo" "$@" --limit 1 --json databaseId --jq '.[0].databaseId'
}
wait_new_run() { # repo, previous id, args...: wait until a newer run exists, then watch it
    local repo=$1 prev=$2 id; shift 2
    for _ in $(seq 60); do
        id=$(latest_run "$repo" "$@")
        [ -n "$id" ] && [ "$id" != "$prev" ] && break
        sleep 5
    done
    [ -n "$id" ] && [ "$id" != "$prev" ] || { echo "no new run in $repo"; exit 1; }
    echo "  run https://github.com/$repo/actions/runs/$id"
    gh run watch "$id" --repo "$repo" --exit-status > /dev/null
}

echo "== $TAG: push and tag"
git push -q origin HEAD
prev=$(latest_run "$PY32" --branch "$TAG")
git tag -a "$TAG" -m "PY32 firmware $TAG"
git push -q origin "$TAG"
wait_new_run "$PY32" "$prev" --branch "$TAG"
gh release view "$TAG" --repo "$PY32" --json assets --jq '.assets[].name' | sed 's/^/  asset /'

echo "== carry into the Zigbee firmware"
prev_bump=$(latest_run "$ZTU" --workflow py32-bump.yml)
prev_build=$(latest_run "$ZTU" --workflow build.yml --branch home-dimmer_build)
if [ -z "${NO_BUMP:-}" ]; then
    gh workflow run py32-bump.yml --repo "$ZTU" --ref home-dimmer -f py32_tag="$TAG"
fi
wait_new_run "$ZTU" "$prev_bump" --workflow py32-bump.yml
echo "== module firmware build"
wait_new_run "$ZTU" "$prev_build" --workflow build.yml --branch home-dimmer_build
echo "== done: $TAG is in the OTA image on home-dimmer_build. Deploy: tools/ha/deploy_ota.sh <ieee>..."
