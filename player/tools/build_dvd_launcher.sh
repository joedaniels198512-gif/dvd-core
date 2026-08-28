#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

#
# Build the SS1 DVD launcher (DDR UI + child-process player + library).
# Same Cortex-A9 / NEON / VFPv3 toolchain as player/build_mac.sh.
# Links libdvdread only (ISO validation). Does NOT link FFmpeg or dvdnav.
# Does NOT compile FPGA.
#

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$ROOT/dist"
DOWNLOADS="$HOME/Downloads"

CROSS="arm-unknown-linux-gnueabihf-"
CC="${CROSS}gcc"
OBJDUMP="${CROSS}objdump"

COMMON_FLAGS="-march=armv7-a -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard"

DVDREAD_DEV="$DOWNLOADS/dvdread-dev-extracted/usr/include"
DVDREAD_SO="$DOWNLOADS/dvdread-extracted/usr/lib/arm-linux-gnueabihf/libdvdread.so.8.0.0"

for t in "$CC" "$OBJDUMP"; do
    command -v "$t" >/dev/null || { echo "Missing tool: $t"; exit 1; }
done

[ -f "$DVDREAD_SO" ] || { echo "ERROR: missing library: $DVDREAD_SO"; exit 1; }
[ -d "$DVDREAD_DEV" ] || { echo "ERROR: missing headers: $DVDREAD_DEV"; exit 1; }

mkdir -p "$OUT"
rm -f "$OUT/dvd_launcher"

echo "Building dvd_launcher..."

"$CC" \
    -O2 \
    -g3 \
    -fno-omit-frame-pointer \
    -Wall -Wextra \
    -std=gnu11 \
    -D_FILE_OFFSET_BITS=64 \
    $COMMON_FLAGS \
    -I"$HERE" \
    -I"$DVDREAD_DEV" \
    -o "$OUT/dvd_launcher" \
    "$HERE/dvd_launcher.c" \
    "$HERE/dvd_library.c" \
    "$DVDREAD_SO" \
    -Wl,-rpath,'$ORIGIN/../lib' \
    -Wl,-no-pie

echo "Checking binary for VFPv4 fused instructions..."

if "$OBJDUMP" -d "$OUT/dvd_launcher" \
    | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' >/dev/null
then
    echo "ERROR: VFPv4 instruction found in binary! Refusing to deploy."
    exit 1
fi

echo "PASS: no VFPv4 fused instructions found."
chmod +x "$OUT/dvd_launcher"
echo
echo "Built: $OUT/dvd_launcher"
echo "Compiler: $($CC --version | head -1)"
echo
echo "Place dvd_launcher next to dvd_av_threaded_test on the SS1."
echo "libdvdread must be available at \$ORIGIN/../lib (usually /media/fat/DVD/lib)."
