#!/bin/bash
set -e

#
# Build, copy and run the MISTER_FB double-buffer filler on the SS1.
#
# PREREQUISITE: the DVD core (built from fpga/DVD.qpf) must be loaded on the
# SS1 first, e.g. by copying DVD.rbf to the SD card and selecting it, or:
#   echo "load_core /media/fat/DVD.rbf" > /dev/MiSTer_cmd
#

HERE="$(cd "$(dirname "$0")" && pwd)"

SS1="root@192.168.1.212"
REMOTE="/media/fat/DVD/dev"

echo "=== Building ==="
"$HERE/build_ddr_writer.sh"

echo
echo "=== Copying to SS1 ==="
ssh "$SS1" "mkdir -p '$REMOTE'"
scp "$HERE/../dist/ddr_fb_double_writer" "$SS1:$REMOTE/ddr_fb_double_writer"

echo
echo "=== Running on SS1 ==="
ssh -t "$SS1" "
    chmod +x '$REMOTE/ddr_fb_double_writer'
    '$REMOTE/ddr_fb_double_writer'
    CODE=\$?
    echo
    echo EXIT CODE=\$CODE
"
