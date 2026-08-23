#!/bin/bash
set -e

#
# Build, copy and run the HPS framebuffer test on the SS1.
# Same target conventions as player/deploy-test.sh.
#
# NOTE: run this while the SS1 is in the MiSTer MENU core, so the
# framebuffer state can be restored after the test (see fb_native_test.c).
#

HERE="$(cd "$(dirname "$0")" && pwd)"

SS1="root@192.168.1.212"
REMOTE="/media/fat/DVD/dev"

echo "=== Building ==="
"$HERE/build_fb_test.sh"

echo
echo "=== Copying to SS1 ==="
ssh "$SS1" "mkdir -p '$REMOTE'"
scp "$HERE/../dist/fb_native_test" "$SS1:$REMOTE/fb_native_test"

echo
echo "=== Running on SS1 ==="
ssh -t "$SS1" "
    chmod +x '$REMOTE/fb_native_test'
    '$REMOTE/fb_native_test'
    CODE=\$?
    echo
    echo EXIT CODE=\$CODE
"
