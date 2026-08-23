#!/bin/bash
set -o pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LOG="$HERE/headless-v0.8.log"
LD_LIBRARY_PATH=/media/fat/DVD/lib \
  "$HERE/dvdplayer_headless" /dev/sr0 2>&1 | tee "$LOG"
status=${PIPESTATUS[0]}
echo "EXIT CODE=$status" | tee -a "$LOG"
exit "$status"
