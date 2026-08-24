#!/bin/bash
set -e

#
# Build, copy and run the mailbox double-buffer DVD video test on the SS1.
#
# PREREQUISITES:
#   1. The mailbox DVD core (tag working-mailbox-double-buffer) must be
#      loaded on the SS1, e.g.:
#        echo "load_core /media/fat/DVD.rbf" > /dev/MiSTer_cmd
#   2. OSD "Buffer" must be left on A.
#   3. A PAL DVD (720x576) must be in the drive at /dev/sr0.
#   4. libdvdnav/libdvdread/libdvdcss at /media/fat/DVD/lib
#

HERE="$(cd "$(dirname "$0")" && pwd)"

SS1="root@192.168.1.212"
REMOTE="/media/fat/DVD/dev"

echo "=== Building ==="
"$HERE/build_dvd_video_double_test.sh"

echo
echo "=== Copying to SS1 ==="
ssh "$SS1" "mkdir -p '$REMOTE'"
scp "$HERE/../dist/dvd_video_double_test" "$SS1:$REMOTE/dvd_video_double_test"

echo
echo "=== Running on SS1 ==="
ssh -t "$SS1" "
    chmod +x '$REMOTE/dvd_video_double_test'
    LD_LIBRARY_PATH=/media/fat/DVD/lib \
    '$REMOTE/dvd_video_double_test' /dev/sr0
    CODE=\$?
    echo
    echo EXIT CODE=\$CODE
"
