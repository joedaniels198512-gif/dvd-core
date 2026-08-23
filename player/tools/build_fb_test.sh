#!/bin/bash
set -euo pipefail

#
# Build the minimal HPS framebuffer test for the SS1.
#
# Uses the same toolchain + CPU constraints as player/build_mac.sh:
#   - arm-unknown-linux-gnueabihf-gcc (NEVER armv7-unknown-linux-gnueabihf!)
#   - Cortex-A9 / NEON / VFPv3 / hard-float, no VFPv4
#

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$ROOT/dist"

CROSS="arm-unknown-linux-gnueabihf-"
CC="${CROSS}gcc"
OBJDUMP="${CROSS}objdump"

COMMON_FLAGS="-march=armv7-a -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard"

for t in "$CC" "$OBJDUMP"; do
    command -v "$t" >/dev/null || {
        echo "Missing tool: $t"
        exit 1
    }
done

mkdir -p "$OUT"
rm -f "$OUT/fb_native_test"

echo "Building fb_native_test..."

"$CC" \
    -O2 \
    -Wall -Wextra \
    -std=gnu11 \
    $COMMON_FLAGS \
    -o "$OUT/fb_native_test" \
    "$HERE/fb_native_test.c"

#
# Same VFPv4 safety gate as build_mac.sh: refuse to ship a binary with
# fused multiply-add instructions the Cortex-A9 cannot execute.
#
echo "Checking binary for VFPv4 fused instructions..."

if "$OBJDUMP" -d "$OUT/fb_native_test" \
    | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' >/dev/null
then
    echo "ERROR: VFPv4 instruction found in binary! Refusing to deploy."
    exit 1
fi

echo "PASS: no VFPv4 fused instructions found."
chmod +x "$OUT/fb_native_test"

echo
echo "Built: $OUT/fb_native_test"
echo "Compiler: $($CC --version | head -1)"
