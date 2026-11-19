#!/bin/bash
# apt-get for the CI runner: a time limit per call and one retry on the other mirror.
#   tools/ci/apt.sh --mirrors     the runner's mirror list -> Azure's Ubuntu mirror first, the main
#                                 archive second (the EC2 region mirror the runner picked stalled
#                                 the toolchain download several times)
#   tools/ci/apt.sh <apt-get args>
set -u
A=http://azure.archive.ubuntu.com/ubuntu/
B=http://archive.ubuntu.com/ubuntu/
LIST=/etc/apt/apt-mirrors.txt   # what the runner's ubuntu.sources point at (mirror+file:)
mirrors() { printf '%s\tpriority:1\n%s\tpriority:2\n' "$1" "$2" | sudo tee "$LIST" > /dev/null; }
run() { timeout 180 sudo apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 "$@"; }

if [ "${1:-}" = --mirrors ]; then
    grep -rq "mirror+file:$LIST" /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null ||
        echo "::warning::the apt sources do not use $LIST: mirror choice has no effect"
    mirrors "$A" "$B"
    exit 0
fi
run "$@" && exit 0
first=$(head -1 "$LIST" 2>/dev/null | cut -f1)
echo "::warning::apt-get $1 failed or stalled (mirror ${first:-?}): once more on the other mirror"
if [ "$first" = "$A" ]; then mirrors "$B" "$A"; else mirrors "$A" "$B"; fi
sleep 5
run "$@"
