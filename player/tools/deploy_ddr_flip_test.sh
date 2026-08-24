#!/bin/bash
set -e

#
# Build, copy and run the ARM mailbox flip test on the SS1.
#
# PREREQUISITE: a DVD.rbf that includes the 0x30400000 mailbox reader
# must be loaded first, e.g.:
#   echo "load_core /media/fat/DVD.rbf" > /dev/MiSTer_cmd
#
# Leave the core OSD "Buffer" option on A so only the mailbox requests flips.
#

HERE="$(cd "$(dirname "$0")" && pwd)"

SS1="root@192.168.1.212"
REMOTE="/media/fat/DVD/dev"

echo "=== Building ==="
"$HERE/build_ddr_flip_test.sh"

echo
echo "=== Copying to SS1 ==="
ssh "$SS1" "mkdir -p '$REMOTE'"
scp "$HERE/../dist/ddr_fb_flip_test" "$SS1:$REMOTE/ddr_fb_flip_test"

echo
echo "=== Running on SS1 ==="
ssh -t "$SS1" "
    chmod +x '$REMOTE/ddr_fb_flip_test'
    '$REMOTE/ddr_fb_flip_test'
    CODE=\$?
    echo
    echo EXIT CODE=\$CODE
"
