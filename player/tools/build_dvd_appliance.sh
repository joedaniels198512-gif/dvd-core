#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

#
# Build the SS1 DVD Appliance supervisor (physical-DVD autoplay, no UI).
# Same Cortex-A9 / NEON / VFPv3 toolchain as player/build_mac.sh.
# Links libdvdread only. Does NOT compile FPGA. Does NOT rebuild the player.
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
rm -f "$OUT/dvd_player" "$OUT/dvd_appliance"

echo "Building dvd_player..."

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
    -o "$OUT/dvd_player" \
    "$HERE/dvd_appliance.c" \
    "$DVDREAD_SO" \
    -Wl,-rpath,'$ORIGIN/../lib' \
    -Wl,-no-pie

echo "Checking binary for VFPv4 fused instructions..."

if "$OBJDUMP" -d "$OUT/dvd_player" \
    | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' >/dev/null
then
    echo "ERROR: VFPv4 instruction found in binary! Refusing to deploy."
    exit 1
fi

echo "PASS: no VFPv4 fused instructions found."
chmod +x "$OUT/dvd_player"
echo
echo "Built: $OUT/dvd_player"
echo "Compiler: $($CC --version | head -1)"
