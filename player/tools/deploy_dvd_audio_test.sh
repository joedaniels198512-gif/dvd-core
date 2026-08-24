#!/bin/bash
set -e

#
# Build, copy and run the DVD audio proof on the SS1.
#
# PREREQUISITES:
#   1. A sys_top core with alsa.sv must be loaded so /dev/MrAudio is
#      consumed at 48 kHz. The mailbox DVD core is fine, e.g.:
#        echo "load_core /media/fat/DVD.rbf" > /dev/MiSTer_cmd
#   2. A DVD must be in the drive at /dev/sr0.
#   3. libdvdnav/libdvdread/libdvdcss at /media/fat/DVD/lib
#   4. HDMI/analog volume up; this test writes PCM only (no video).
#

HERE="$(cd "$(dirname "$0")" && pwd)"

SS1="root@192.168.1.212"
REMOTE="/media/fat/DVD/dev"

echo "=== Building ==="
"$HERE/build_dvd_audio_test.sh"

echo
echo "=== Copying to SS1 ==="
ssh "$SS1" "mkdir -p '$REMOTE'"
scp "$HERE/../dist/dvd_audio_test" "$SS1:$REMOTE/dvd_audio_test"

echo
echo "=== Running on SS1 ==="
ssh -t "$SS1" "
    chmod +x '$REMOTE/dvd_audio_test'
    LD_LIBRARY_PATH=/media/fat/DVD/lib \
    '$REMOTE/dvd_audio_test' /dev/sr0
    CODE=\$?
    echo
    echo EXIT CODE=\$CODE
"
