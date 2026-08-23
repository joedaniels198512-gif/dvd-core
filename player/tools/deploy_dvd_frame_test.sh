#!/bin/bash
set -e

#
# Build, copy and run the single-frame DVD -> MISTER_FB test on the SS1.
#
# PREREQUISITES:
#   1. The DVD core (built from fpga/DVD.qpf, tag working-mister-fb) must be
#      loaded on the SS1, e.g.:
#        echo "load_core /media/fat/DVD.rbf" > /dev/MiSTer_cmd
#   2. A PAL DVD (720x576) must be in the drive at /dev/sr0.
#   3. libdvdnav/libdvdread/libdvdcss must be at /media/fat/DVD/lib
#      (same as the main player, see player/deploy-test.sh).
#

HERE="$(cd "$(dirname "$0")" && pwd)"

SS1="root@192.168.1.212"
REMOTE="/media/fat/DVD/dev"

echo "=== Building ==="
"$HERE/build_dvd_frame_test.sh"

echo
echo "=== Copying to SS1 ==="
ssh "$SS1" "mkdir -p '$REMOTE'"
scp "$HERE/../dist/dvd_frame_test" "$SS1:$REMOTE/dvd_frame_test"

echo
echo "=== Running on SS1 ==="
ssh -t "$SS1" "
    chmod +x '$REMOTE/dvd_frame_test'
    LD_LIBRARY_PATH=/media/fat/DVD/lib \
    '$REMOTE/dvd_frame_test' /dev/sr0
    CODE=\$?
    echo
    echo EXIT CODE=\$CODE
"
