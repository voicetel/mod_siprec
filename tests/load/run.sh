#!/usr/bin/env bash
# Host orchestration for the Layer 3 load gate. Snapshots the CURRENT
# mod_siprec working tree (so uncommitted changes UNDER TEST are what
# build), assembles a Docker build context with callBroadcast's FreeSWITCH
# installer as the build engine, builds the image, and runs the gate.
#
#   tests/load/run.sh
#
# Env:
#   CBFS   path to callBroadcast's freeswitch installer dir
#          (default: ../callBroadcast/freeswitch relative to this repo)
#   TAG    image tag (default: mod-siprec-load)
#
# The build is a full FreeSWITCH-from-source compile (~30-45 min at JOBS=2)
# and produces a multi-GB image; both are deleted-able afterward with
#   docker image rm "$TAG"
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)   # tests/load
repo=$(cd "$here/../.." && pwd)       # repo root (two levels up)
tag=${TAG:-mod-siprec-load}
cbfs=${CBFS:-$(cd "$repo/../callBroadcast/freeswitch" 2>/dev/null && pwd || true)}

if [ -z "$cbfs" ] || [ ! -f "$cbfs/install-freeswitch-debian-trixie.sh" ]; then
    echo "callBroadcast FS installer not found." >&2
    echo "Set CBFS=/path/to/callBroadcast/freeswitch" >&2
    exit 1
fi

ctx=$(mktemp -d)
trap 'rm -rf "$ctx"' EXIT

echo "==> snapshotting working tree (code under test) into build context"
mkdir -p "$ctx/mod_siprec_snapshot"
rsync -a --exclude='.git' --exclude='*.o' --exclude='*.gcno' --exclude='*.gcda' \
      --exclude='*.gcov' --exclude='siprec_test' --exclude='tests/load' \
      "$repo/" "$ctx/mod_siprec_snapshot/"
git -C "$ctx/mod_siprec_snapshot" init -q
git -C "$ctx/mod_siprec_snapshot" add -A
git -C "$ctx/mod_siprec_snapshot" -c user.email=load@test -c user.name=load \
    commit -qm 'working-tree snapshot under test'
ref=$(git -C "$ctx/mod_siprec_snapshot" rev-parse HEAD)

echo "==> assembling context (installer engine + scripts)"
cp -a "$cbfs" "$ctx/cbfs"
cp "$here/Dockerfile" "$here/fsbuild.sh" "$here/gate.sh" "$ctx/"

if [ "${DRY_RUN:-0}" = 1 ]; then
    echo "DRY_RUN: context=$ctx ref=$ref cbfs=$cbfs tag=$tag — skipping docker build"
    ls "$ctx"
    exit 0
fi

echo "==> docker build (FreeSWITCH + sofia + mod_siprec; ~30-45 min, JOBS=2)"
docker build --build-arg "SIPREC_REF=$ref" -t "$tag" "$ctx"

echo "==> running load gate"
# --cap-add=SYS_NICE: FreeSWITCH sets SCHED_FIFO / nice at startup, which a
# default unprivileged container forbids ("Operation not permitted").
docker run --rm --cap-add=SYS_NICE "$tag"
