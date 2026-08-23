#!/bin/bash
set -e

SS1="root@192.168.1.212"
REMOTE="/media/fat/DVD/dev"

echo "=== Building ==="
./build_mac.sh

echo
echo "=== Copying to SS1 ==="
ssh "$SS1" "mkdir -p '$REMOTE'"

scp dist/dvdplayer_headless \
    "$SS1:$REMOTE/dvdplayer_headless"

echo
echo "=== Running on SS1 ==="
ssh -t "$SS1" "
    chmod +x '$REMOTE/dvdplayer_headless'
    LD_LIBRARY_PATH=/media/fat/DVD/lib \
    '$REMOTE/dvdplayer_headless' /dev/sr0
    CODE=\$?
    echo
    echo EXIT CODE=\$CODE
"

