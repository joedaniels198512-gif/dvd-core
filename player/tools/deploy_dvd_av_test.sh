#!/bin/bash
set -e

#
# Build, copy and run the first combined DVD A/V proof on the SS1.
#
# PREREQUISITES:
#   1. Mailbox DVD core loaded, e.g.:
#        echo "load_core /media/fat/DVD.rbf" > /dev/MiSTer_cmd
#   2. OSD Buffer left on A.
#   3. PAL DVD in /dev/sr0.
#   4. libdvdnav/libdvdread/libdvdcss at /media/fat/DVD/lib
#   5. HDMI/analog volume up.
#

HERE="$(cd "$(dirname "$0")" && pwd)"

SS1="root@192.168.1.212"
REMOTE="/media/fat/DVD/dev"

echo "=== Building ==="
"$HERE/build_dvd_av_test.sh"

echo
echo "=== Copying to SS1 ==="
ssh "$SS1" "mkdir -p '$REMOTE'"
scp "$HERE/../dist/dvd_av_test" "$SS1:$REMOTE/dvd_av_test"

echo
echo "=== Running on SS1 ==="
ssh -t "$SS1" "
    chmod +x '$REMOTE/dvd_av_test'
    LD_LIBRARY_PATH=/media/fat/DVD/lib \
    '$REMOTE/dvd_av_test' /dev/sr0
    CODE=\$?
    echo
    echo EXIT CODE=\$CODE
"
