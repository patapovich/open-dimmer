#!/bin/bash
# Release the PY32 firmware end to end:
#   checks (clean tree, host tests, build) -> tag vX.Y.Z from FW_VERSION -> CI builds and
#   publishes the release -> py32-bump in patapovich/tuya-zigbee-switch carries it into
#   home-dimmer -> the module firmware (OTA image) is built on home-dimmer_build.
# The carrying runs in the fork's py32-bump workflow (cloud); LOCAL_BUMP=1 does it here
# in ~/projects/tzs-diag instead, NO_TRIGGER=1 only waits (the release dispatched it).
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
if git rev-parse -q --verify "refs/tags/$TAG" > /dev/null &&
   [ "$(git rev-parse "$TAG^{commit}")" != "$(git rev-parse HEAD)" ]; then
    # the tag exists but the code has moved on: a new version is needed
    if [ -n "$(git diff --name-only "$TAG" HEAD -- src boot Makefile py32f002a_app.ld)" ]; then
        echo "$TAG is released and the firmware changed since: bump FW_VERSION in src/tuya.h"; exit 1
    fi
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
    LAST_RUN=$id
    gh run watch "$id" --repo "$repo" --exit-status > /dev/null
}

TMP=$(mktemp -d)
ZTU_DIR=~/projects/tzs-diag
VENV_PY=~/projects/tuya-zigbee-switch/.venv/bin/python

if gh release view "$TAG" --repo "$PY32" > /dev/null 2>&1; then
    echo "== $TAG already released: carrying it on"
else
    echo "== $TAG: push and tag"
    git push -q origin HEAD
    prev=$(latest_run "$PY32" --branch "$TAG")
    git tag -a "$TAG" -m "PY32 firmware $TAG"
    git push -q origin "$TAG"
    wait_new_run "$PY32" "$prev" --branch "$TAG"
fi
gh release view "$TAG" --repo "$PY32" --json assets --jq '.assets[].name' | sed 's/^/  asset /'

if [ -z "${LOCAL_BUMP:-}" ]; then
    echo "== carry into the Zigbee firmware: py32-bump in $ZTU (cloud)"
    prev_bump=$(latest_run "$ZTU" --workflow py32-bump.yml)
    prev_build=$(latest_run "$ZTU" --workflow build.yml --branch home-dimmer_build)
    [ -n "${NO_TRIGGER:-}" ] || gh workflow run py32-bump.yml --repo "$ZTU" -f py32_tag="$TAG"
    wait_new_run "$ZTU" "$prev_bump" --workflow py32-bump.yml
    built=$(gh run view "$LAST_RUN" --repo "$ZTU" --json jobs \
        --jq '.jobs[0].steps[] | select(.name == "Build branch and module firmware") | .conclusion')
    if [ "$built" = "success" ]; then
        echo "== module firmware build"
        wait_new_run "$ZTU" "$prev_build" --workflow build.yml --branch home-dimmer_build
    else
        echo "  home-dimmer already carried $TAG: no new module build"
    fi
else
    echo "== carry into the Zigbee firmware (home-dimmer in $ZTU_DIR)"
    [ "$(git -C "$ZTU_DIR" branch --show-current)" = "home-dimmer" ] || { echo "$ZTU_DIR is not on home-dimmer"; exit 1; }
    [ -z "$(git -C "$ZTU_DIR" status --porcelain --untracked-files=no)" ] || { echo "$ZTU_DIR has uncommitted changes"; exit 1; }
    gh release download "$TAG" --repo "$PY32" --pattern secondary_mcu_image.c --pattern SHA256SUMS --dir "$TMP"
    (cd "$TMP" && grep secondary_mcu_image.c SHA256SUMS | sha256sum -c --quiet -)
    cp "$TMP/secondary_mcu_image.c" "$ZTU_DIR/src/zigbee/secondary_mcu_image.c"
    (
        cd "$ZTU_DIR"
        find src -name '*.c' -o -name '*.h' | xargs uncrustify -c uncrustify.cfg --check -q
        make -C src/stub build -s > /dev/null
        "$VENV_PY" -m pytest tests/ -q -p no:cacheprovider 2>&1 | tail -1
        if git diff --quiet; then
            echo "  home-dimmer already carries $TAG"
        else
            git commit -qam "Carry PY32 $TAG"
            git push -q patapovich home-dimmer
            echo "  committed: $(git log --oneline -1)"
        fi
        git fetch -q patapovich fork-ci
        git branch -f home-dimmer_build home-dimmer
        git switch -q home-dimmer_build
        git cherry-pick patapovich/fork-ci > /dev/null
        git push -q -f patapovich home-dimmer_build
        git switch -q home-dimmer
    )

    echo "== module firmware build"
    prev_build=$(latest_run "$ZTU" --workflow build.yml --branch home-dimmer_build)
    gh workflow run build.yml --repo "$ZTU" --ref home-dimmer_build
    wait_new_run "$ZTU" "$prev_build" --workflow build.yml --branch home-dimmer_build
fi
echo "== done: $TAG is in the OTA image on home-dimmer_build. Deploy: tools/ha/deploy_ota.sh <ieee>..."
