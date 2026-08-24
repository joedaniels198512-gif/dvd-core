#!/bin/bash
set -euo pipefail

#
# Build the continuous-video DVD -> MISTER_FB throughput test for the SS1.
#
# Same toolchain + CPU constraints as player/build_mac.sh:
#   arm-unknown-linux-gnueabihf-gcc, Cortex-A9 / NEON / VFPv3, no VFPv4.
#
# Reuses the FFmpeg static libraries and dvdnav/dvdread shared objects that
# player/build_mac.sh already builds/uses. It does NOT rebuild FFmpeg: run
# player/build_mac.sh once first if the libraries are missing.
#

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$ROOT/dist"
DOWNLOADS="$HOME/Downloads"

CROSS="arm-unknown-linux-gnueabihf-"
CC="${CROSS}gcc"
OBJDUMP="${CROSS}objdump"

COMMON_FLAGS="-march=armv7-a -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard"

# Same locations as player/build_mac.sh.
FF_PREFIX="$ROOT/.build/ffmpeg-ss1-cortex-a9-video"

DVDNAV_DEV="$DOWNLOADS/dvdnav-dev-extracted/usr/include"
DVDREAD_DEV="$DOWNLOADS/dvdread-dev-extracted/usr/include"

DVDNAV_SO="$DOWNLOADS/dvdnav-lib-extracted/usr/lib/arm-linux-gnueabihf/libdvdnav.so.4.3.0"
DVDREAD_SO="$DOWNLOADS/dvdread-extracted/usr/lib/arm-linux-gnueabihf/libdvdread.so.8.0.0"

for t in "$CC" "$OBJDUMP"; do
    command -v "$t" >/dev/null || {
        echo "Missing tool: $t"
        exit 1
    }
done

if [ ! -f "$FF_PREFIX/lib/libavformat.a" ]; then
    echo "ERROR: VFPv3-safe FFmpeg libraries not found at:"
    echo "  $FF_PREFIX"
    echo "Run player/build_mac.sh once first to build them."
    exit 1
fi

for f in "$DVDNAV_SO" "$DVDREAD_SO"; do
    [ -f "$f" ] || {
        echo "ERROR: missing library: $f"
        exit 1
    }
done

mkdir -p "$OUT"
rm -f "$OUT/dvd_video_test"

echo "Building dvd_video_test..."

"$CC" \
    -O2 \
    -g3 \
    -fno-omit-frame-pointer \
    -Wall -Wextra \
    -std=gnu11 \
    $COMMON_FLAGS \
    -I"$FF_PREFIX/include" \
    -I"$DVDNAV_DEV" \
    -I"$DVDREAD_DEV" \
    -o "$OUT/dvd_video_test" \
    "$HERE/dvd_video_test.c" \
    "$DVDNAV_SO" \
    "$DVDREAD_SO" \
    -Wl,--start-group \
        "$FF_PREFIX/lib/libavformat.a" \
        "$FF_PREFIX/lib/libavcodec.a" \
        "$FF_PREFIX/lib/libswscale.a" \
        "$FF_PREFIX/lib/libavutil.a" \
    -Wl,--end-group \
    -Wl,-rpath,'$ORIGIN/../lib' \
    -Wl,-no-pie \
    -pthread \
    -lm \
    -ldl \
    -latomic

#
# Critical safety check: VFMA/VFMS/VFNMA/VFNMS are VFPv4 instructions.
# The SS1 Cortex-A9 only has VFPv3.
#
echo "Checking binary for VFPv4 fused instructions..."

if "$OBJDUMP" -d "$OUT/dvd_video_test" \
    | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' >/dev/null
then
    echo "ERROR: VFPv4 instruction found in binary! Refusing to deploy."
    exit 1
fi

echo "PASS: no VFPv4 fused instructions found."
chmod +x "$OUT/dvd_video_test"

echo
echo "Built: $OUT/dvd_video_test"
echo "Compiler: $($CC --version | head -1)"
