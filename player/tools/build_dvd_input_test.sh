#!/bin/bash
set -euo pipefail

#
# Build the FPGA→ARM logical controller proof for the SS1.
# Cortex-A9 / NEON / VFPv3 only. Does not link FFmpeg. Does not compile FPGA.
#

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$ROOT/dist"

CROSS="arm-unknown-linux-gnueabihf-"
CC="${CROSS}gcc"
OBJDUMP="${CROSS}objdump"

COMMON_FLAGS="-march=armv7-a -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard"

for t in "$CC" "$OBJDUMP"; do
    command -v "$t" >/dev/null || { echo "Missing tool: $t"; exit 1; }
done

mkdir -p "$OUT"
rm -f "$OUT/dvd_input_test"

echo "Building dvd_input_test..."

"$CC" \
    -O2 \
    -Wall -Wextra \
    -std=gnu11 \
    $COMMON_FLAGS \
    -o "$OUT/dvd_input_test" \
    "$HERE/dvd_input_test.c"

echo "Checking binary for VFPv4 fused instructions..."

if "$OBJDUMP" -d "$OUT/dvd_input_test" \
    | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' >/dev/null
then
    echo "ERROR: VFPv4 instruction found in binary! Refusing to deploy."
    exit 1
fi

echo "PASS: no VFPv4 fused instructions found."
chmod +x "$OUT/dvd_input_test"
echo
echo "Built: $OUT/dvd_input_test"
echo "Compiler: $($CC --version | head -1)"
