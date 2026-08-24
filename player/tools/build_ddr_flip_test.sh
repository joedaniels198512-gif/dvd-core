#!/bin/bash
set -euo pipefail

#
# Build the ARM mailbox double-buffer flip test for the SS1.
# Same toolchain + CPU constraints as player/build_mac.sh:
#   arm-unknown-linux-gnueabihf-gcc, Cortex-A9 / NEON / VFPv3, no VFPv4.
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
rm -f "$OUT/ddr_fb_flip_test"

echo "Building ddr_fb_flip_test..."

"$CC" \
    -O2 \
    -Wall -Wextra \
    -std=gnu11 \
    $COMMON_FLAGS \
    -o "$OUT/ddr_fb_flip_test" \
    "$HERE/ddr_fb_flip_test.c"

echo "Checking binary for VFPv4 fused instructions..."

if "$OBJDUMP" -d "$OUT/ddr_fb_flip_test" \
    | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' >/dev/null
then
    echo "ERROR: VFPv4 instruction found in binary! Refusing to deploy."
    exit 1
fi

echo "PASS: no VFPv4 fused instructions found."
chmod +x "$OUT/ddr_fb_flip_test"

echo
echo "Built: $OUT/ddr_fb_flip_test"
echo "Compiler: $($CC --version | head -1)"
