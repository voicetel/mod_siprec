#!/usr/bin/env bash
# Live SIPREC test (Layer 4). Runs live.sh inside the image that
# tests/load/run.sh builds (FreeSWITCH + sofia + this tree's mod_siprec),
# with this directory mounted read-only. Build the image first.
#
#   tests/live/run.sh
#
# Env:
#   TAG   image tag (default: mod-siprec-load, as tests/load/run.sh uses)
#   OUT   host directory for artifacts: fs.log and the SRS captures
#         (default: a fresh temp dir, printed at the end)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
tag=${TAG:-mod-siprec-load}
out=${OUT:-$(mktemp -d)}
chmod 777 "$out"

rc=0
docker run --rm --cap-add=SYS_NICE \
    -v "$here:/live:ro" -v "$out:/tmp/live" \
    "$tag" bash /live/live.sh || rc=$?
echo "artifacts: $out"
exit $rc
