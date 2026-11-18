#!/bin/bash
# Release the PY32 firmware end to end:
#   checks (on main, clean, origin/main contained; host tests; normal, DIAG, MEASURE and SIM
#   builds) -> tag vX.Y.Z from FW_VERSION -> CI tests, builds and publishes the release ->
#   py32-bump in patapovich/tuya-zigbee-switch carries it into home-dimmer -> the module
#   firmware (OTA image) is built on home-dimmer_build.
# The carrying runs in the fork's py32-bump workflow (cloud); LOCAL_BUMP=1 does it here
# in ~/projects/tzs-diag instead, NO_TRIGGER=1 only waits for the py32-bump run that the
# release's hand-over job starts (secret ZTU_DISPATCH_TOKEN).
# Running it again resumes: a tag whose CI run failed gets its failed jobs re-run, a release
# with a green CI run is carried on; a tag is never moved or deleted here. The CI run counts
# as green when its tests, builds and release succeeded: the optional hand-over job may fail
# (expired token), then this script starts py32-bump itself.
# Deploying to modules is a separate, local step: tools/ha/deploy_ota.sh <ieee | friendly_name:ieee>...
# Usage: tools/release/release.sh            (bump FW_VERSION in src/tuya.h first)
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/../.."
PY32=patapovich/zdms16-py32
ZTU=patapovich/tuya-zigbee-switch
# Everything that ends up in the released files: a change here needs a new version.
FW_PATHS=(src boot Makefile py32f002a_app.ld tools/mkimage.py tools/embed_ztu.py)

die() { echo "$*" >&2; exit 1; }

# --- the source -----------------------------------------------------------------------------
[ "$(git branch --show-current)" = main ] || die "not on main"
[ -z "$(git status --porcelain)" ] || die "uncommitted changes"
git fetch -q origin main
git merge-base --is-ancestor origin/main HEAD || die "origin/main is not contained in HEAD: rebase on origin/main first"
SHA=$(git rev-parse HEAD)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- tests and all four builds, in fresh build dirs (objects of an earlier VERSION_BYTE
#     build cannot mix in) ---------------------------------------------------------------------
make test > /dev/null
for v in "" DIAG=1 MEASURE=1 SIM=1; do
    make ${v:+"$v"} BUILD="$TMP/build${v:+-${v%=1}}" > /dev/null || die "make $v failed"
done
vf=$(python3 -c "import sys; d=open(sys.argv[1],'rb').read(); print(d[0xCC], d[0xCD])" "$TMP/build/zdms16_py32.img")
read -r VB FLAGS <<< "$vf"
[ "$FLAGS" = 0 ] || die "the normal build has image flags $FLAGS (SIM/MEASURE)"
case $VB in
    191) die "version byte 0xBF (2.3.15) is reserved for the SIM bench build: pick another FW_VERSION" ;;
    255) die "version byte 0xFF: no update could ever follow it: pick another FW_VERSION" ;;
esac
TAG=$(printf 'v%d.%d.%d' $((VB >> 6)) $(((VB >> 4) & 3)) $((VB & 15)))
limit=$(( VB < 0xBF ? 0xBF : 0xFF ))
left=$(( limit - VB - 1 ))
[ "$left" -ge 16 ] || printf 'warning: %s (0x%02X) leaves %d version codes before 0x%02X\n' "$TAG" "$VB" "$left" "$limit" >&2

# --- GitHub runs ------------------------------------------------------------------------------
max_run_id() {   # repo, gh run list args...: id of the newest run, 0 if none
    local repo=$1; shift
    gh run list --repo "$repo" "$@" --limit 1 --json databaseId --jq '.[0].databaseId // 0'
}
find_run() {     # repo, ids above, jq condition, gh run list args...: newest matching run id
    local repo=$1 above=$2 cond=$3; shift 3
    gh run list --repo "$repo" "$@" --limit 20 \
        --json databaseId,headSha,event,status,conclusion,displayTitle,createdAt \
        --jq "[.[] | select(.databaseId > $above) | select($cond)] | .[0].databaseId // empty"
}
watch_run() {    # repo, id, what: wait for the run, fail unless it succeeded
    echo "  $3 run https://github.com/$1/actions/runs/$2"
    LAST_RUN=$2
    gh run watch "$2" --repo "$1" --exit-status > /dev/null ||
        die "$3 run failed: https://github.com/$1/actions/runs/$2"
}
await_id() {     # repo, what, ids above, jq condition, gh run list args...: wait for it to appear (sets RUN_ID)
    local repo=$1 what=$2 above=$3 cond=$4 id=""; shift 4
    for _ in $(seq 60); do
        id=$(find_run "$repo" "$above" "$cond" "$@")
        [ -n "$id" ] && break
        sleep 5
    done
    [ -n "$id" ] || die "no $what run appeared in $repo"
    RUN_ID=$id
}
await_run() {    # the same arguments: wait for the run to appear, watch it
    await_id "$@"
    watch_run "$1" "$RUN_ID" "$2"
}
dispatched_id() { # output of gh workflow run: the run id, if it names the run
    grep -o 'actions/runs/[0-9]*' <<< "$1" | head -1 | cut -d/ -f3 || true
}
ensure_enabled() { # repo, workflow file: GitHub disables scheduled workflows after 60 idle days
    local state
    state=$(gh api "repos/$1/actions/workflows/$2" --jq .state)
    case $state in
        active) ;;
        disabled_inactivity)
            echo "  $2 in $1 was disabled for inactivity: enabling it"
            gh api -X PUT "repos/$1/actions/workflows/$2/enable" > /dev/null ;;
        *) die "$2 in $1 is $state: enable it by hand if it should run" ;;
    esac
}
tag_run() {      # commit: "id status" of the newest CI run of the tag push at it
    gh run list --repo "$PY32" --workflow ci.yml --event push --branch "$TAG" --commit "$1" --limit 1 \
        --json databaseId,status --jq '.[0] | select(.) | "\(.databaseId) \(.status)"'
}
ci_url() { echo "https://github.com/$PY32/actions/runs/$1"; }
watch_ci() {     # run id: wait for the tag's CI run to finish; ci_judge decides, not its conclusion
    echo "  CI ($TAG) run $(ci_url "$1")"
    gh run watch "$1" --repo "$PY32" > /dev/null || true
}
ci_judge() {     # run id: CI_OK=1 when every job but the optional hand-over succeeded (or was
                 # skipped); HANDED_OVER=1 when the hand-over job succeeded
    local j status nfail handover
    j=$(gh run view "$1" --repo "$PY32" --json status,jobs --jq '
        ([.jobs[] | select(.name | startswith("Hand over"))] | .[0].conclusion // "" |
            if . == "" then "none" else . end) as $h
        | ([.jobs[] | select(.name | startswith("Hand over") | not)
            | select(.conclusion != "success" and .conclusion != "skipped")] | length) as $n
        | "\(.status) \($n) \($h)"') || die "cannot read the CI ($TAG) run $(ci_url "$1")"
    read -r status nfail handover <<< "$j"
    [ "$status" = completed ] || die "CI ($TAG) run is $status: $(ci_url "$1"): run this again later"
    CI_OK="" HANDED_OVER=""
    [ "$nfail" = 0 ] && CI_OK=1
    case $handover in
        success) HANDED_OVER=1 ;;
        none | skipped) ;;
        *) [ -z "$CI_OK" ] ||
            echo "warning: the hand-over job of the CI ($TAG) run did not succeed ($handover): ZTU_DISPATCH_TOKEN expired or without Actions write, or py32-bump disabled? $(ci_url "$1")" >&2 ;;
    esac
}
move_tag() {     # how to move the unreleased tag to HEAD
    printf '%s\n  %s' "move the unreleased tag (nobody uses it) to HEAD:" \
        "git push origin :refs/tags/$TAG && git tag -d $TAG    and run this again"
}
ci_failed() {    # run id: stop after a failed CI run of the tag, with the ways on
    local msg
    msg="CI ($TAG) run failed: $(ci_url "$1")"
    [ "$(release_state)" != none ] || msg+="
By chance (runner, network): run this again, it re-runs the failed jobs.
In the code or the workflow: fix it on main, then $(move_tag)"
    die "$msg"
}
rerun_ci() {     # run id: re-run the failed jobs of a finished CI run and wait for it
    echo "  CI ($TAG) run $1 did not succeed: re-running its failed jobs"
    gh run rerun "$1" --repo "$PY32" --failed
    for _ in $(seq 30); do
        [ "$(gh run view "$1" --repo "$PY32" --json status --jq .status)" != completed ] && break
        sleep 2
    done
    watch_ci "$1"
}
release_state() { # none, draft or published
    local d
    d=$(gh release view "$TAG" --repo "$PY32" --json isDraft --jq .isDraft 2> /dev/null) || { echo none; return; }
    [ "$d" = true ] && echo draft || echo published
}

# Runs that a release may start: read before anything is pushed (NO_TRIGGER waits for the
# py32-bump run the release's hand-over job starts).
prev_bump=$(max_run_id "$ZTU" --workflow py32-bump.yml)
prev_build=$(max_run_id "$ZTU" --workflow build.yml --branch home-dimmer_build)
CI_RAN=""        # a CI run of the tag finished during this call
HANDED_OVER=""   # and its hand-over job succeeded (it started py32-bump, if it had the token)

# --- tag, CI, release -------------------------------------------------------------------------
remote_tag=$(git ls-remote origin "refs/tags/$TAG" "refs/tags/$TAG^{}" | awk 'END { print $1 }')
local_tag=$(git rev-parse -q --verify "refs/tags/$TAG^{commit}" || true)
REL=$(release_state)

if [ -n "$remote_tag" ]; then
    if [ -z "$local_tag" ]; then
        git fetch -q origin "refs/tags/$TAG:refs/tags/$TAG"
        local_tag=$(git rev-parse "refs/tags/$TAG^{commit}")
    fi
    [ "$local_tag" = "$remote_tag" ] ||
        die "tag $TAG here (${local_tag:0:9}) differs from origin's (${remote_tag:0:9})"
    if [ "$remote_tag" != "$SHA" ] && [ -n "$(git diff --name-only "$remote_tag" HEAD -- "${FW_PATHS[@]}")" ]; then
        # the tag exists but the firmware has moved on: a new version is needed
        [ "$REL" != none ] && die "$TAG is released and the firmware changed since: bump FW_VERSION in src/tuya.h"
        die "$TAG is on origin at ${remote_tag:0:9} without a release, and the firmware changed since.
Bump FW_VERSION in src/tuya.h, or $(move_tag)"
    fi
    # The release counts only with a green CI run of the tag at its commit.
    run=$(tag_run "$remote_tag")
    [ -n "$run" ] || die "no CI run of $TAG at ${remote_tag:0:9} in $PY32: push the tag again (git push origin :refs/tags/$TAG && git push origin $TAG)"
    read -r id status <<< "$run"
    if [ "$status" != completed ]; then
        echo "== $TAG: CI run in progress"
        watch_ci "$id"
        CI_RAN=1
        ci_judge "$id"
        [ -n "$CI_OK" ] || ci_failed "$id"
    else
        ci_judge "$id"
        if [ -z "$CI_OK" ]; then
            echo "== $TAG: CI run failed"
            # A re-run runs the tag's commit (its code, tests and workflow), not HEAD.
            if [ "$REL" = none ] && [ "$remote_tag" != "$SHA" ]; then
                changed=$(git diff --name-only "$remote_tag" HEAD -- . ':!docs' ':!*.md' ':!LICENSE')
                [ -z "$changed" ] || die "$TAG has no release, its CI run failed ($(ci_url "$id")), and HEAD changed since the tag:
$(head -5 <<< "$changed" | sed 's/^/  /')
A re-run would run the tag's old code and workflow again: $(move_tag)"
            fi
            rerun_ci "$id"
            CI_RAN=1
            ci_judge "$id"
            [ -n "$CI_OK" ] || ci_failed "$id"
        fi
    fi
    REL=$(release_state)
    [ "$REL" = published ] || die "the CI run of $TAG succeeded but the release is $REL: see https://github.com/$PY32/releases"
    if [ -n "$CI_RAN" ]; then
        echo "== $TAG released"
    else
        echo "== $TAG already released (green CI run): carrying it on"
    fi
else
    [ "$REL" = none ] || die "release $TAG exists ($REL) but origin has no tag $TAG: sort it out on GitHub first"
    if [ -n "$local_tag" ] && [ "$local_tag" != "$SHA" ]; then
        die "tag $TAG here is at ${local_tag:0:9}, not HEAD, and not on origin: git tag -d $TAG and run this again"
    fi
    echo "== $TAG: push and tag"
    prev_ci=$(max_run_id "$PY32" --workflow ci.yml --branch "$TAG")
    git push -q origin HEAD:main
    [ -n "$local_tag" ] || git tag -a "$TAG" -m "PY32 firmware $TAG"
    git push -q origin "refs/tags/$TAG"
    await_id "$PY32" "CI ($TAG)" "$prev_ci" '.event == "push"' \
        --workflow ci.yml --event push --branch "$TAG" --commit "$SHA"
    watch_ci "$RUN_ID"
    CI_RAN=1
    ci_judge "$RUN_ID"
    [ -n "$CI_OK" ] || ci_failed "$RUN_ID"
    [ "$(release_state)" = published ] || die "the CI run of $TAG succeeded but there is no published release"
fi
gh release view "$TAG" --repo "$PY32" --json assets --jq '.assets[].name' | sed 's/^/  asset /'

# --- carry into the Zigbee firmware -----------------------------------------------------------
ZTU_DIR=~/projects/tzs-diag
VENV_PY=~/projects/tuya-zigbee-switch/.venv/bin/python
# py32-bump runs are titled "Carry PY32 <tag>" (older ones: the workflow name)
bump_cond=".event != \"schedule\" and ((.displayTitle | startswith(\"Carry PY32 \") | not) or .displayTitle == \"Carry PY32 $TAG\")"

if [ -z "${LOCAL_BUMP:-}" ]; then
    echo "== carry into the Zigbee firmware: py32-bump in $ZTU (cloud)"
    if [ -n "${NO_TRIGGER:-}" ] && [ -n "$CI_RAN" ] && [ -n "$HANDED_OVER" ]; then
        await_run "$ZTU" "py32-bump" "$prev_bump" "$bump_cond" --workflow py32-bump.yml
    else
        if [ -n "${NO_TRIGGER:-}" ]; then
            if [ -n "$CI_RAN" ]; then
                echo "  (NO_TRIGGER, but the hand-over job did not succeed: starting py32-bump here)"
            else
                echo "  (NO_TRIGGER, but no CI run of $TAG finished now: starting py32-bump here)"
            fi
        fi
        ensure_enabled "$ZTU" py32-bump.yml
        prev_bump=$(max_run_id "$ZTU" --workflow py32-bump.yml)
        out=$(gh workflow run py32-bump.yml --repo "$ZTU" -f py32_tag="$TAG" 2>&1) || die "$out"
        id=$(dispatched_id "$out")
        if [ -n "$id" ]; then
            watch_run "$ZTU" "$id" "py32-bump"
        else
            await_run "$ZTU" "py32-bump" "$prev_bump" ".event == \"workflow_dispatch\" and ($bump_cond)" \
                --workflow py32-bump.yml
        fi
    fi
    built=$(gh run view "$LAST_RUN" --repo "$ZTU" --json jobs \
        --jq '[.jobs[].steps[] | select(.name == "Build branch and module firmware") | .conclusion] | .[0] // empty')
    if [ "$built" = "success" ]; then
        echo "== module firmware build"
        since=$(gh run view "$LAST_RUN" --repo "$ZTU" --json createdAt --jq .createdAt)
        await_run "$ZTU" "module firmware build" "$prev_build" \
            ".event == \"workflow_dispatch\" and .createdAt >= \"$since\"" \
            --workflow build.yml --branch home-dimmer_build
    else
        echo "  home-dimmer already carried $TAG: no new module build"
    fi
else
    echo "== carry into the Zigbee firmware (home-dimmer in $ZTU_DIR)"
    [ "$(git -C "$ZTU_DIR" branch --show-current)" = "home-dimmer" ] || die "$ZTU_DIR is not on home-dimmer"
    [ -z "$(git -C "$ZTU_DIR" status --porcelain --untracked-files=no)" ] || die "$ZTU_DIR has uncommitted changes"
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
    build_sha=$(git -C "$ZTU_DIR" rev-parse home-dimmer_build)
    prev_build=$(max_run_id "$ZTU" --workflow build.yml --branch home-dimmer_build)
    out=$(gh workflow run build.yml --repo "$ZTU" --ref home-dimmer_build 2>&1) || die "$out"
    id=$(dispatched_id "$out")
    if [ -n "$id" ]; then
        watch_run "$ZTU" "$id" "module firmware build"
    else
        await_run "$ZTU" "module firmware build" "$prev_build" \
            ".event == \"workflow_dispatch\" and .headSha == \"$build_sha\"" \
            --workflow build.yml --branch home-dimmer_build
    fi
fi
echo "== done: $TAG is in the OTA image on home-dimmer_build. Deploy: tools/ha/deploy_ota.sh <ieee | friendly_name:ieee>... (tools/ha/README.md)"
