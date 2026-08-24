#!/bin/bash
set -euo pipefail

#
# Build the DVD -> /dev/MrAudio audio proof for the SS1.
#
# Same toolchain + CPU constraints as player/build_mac.sh:
#   arm-unknown-linux-gnueabihf-gcc, Cortex-A9 / NEON / VFPv3, no VFPv4.
#
# Reuses the FFmpeg static libraries (including libswresample, already
# present in this prefix) and dvdnav/dvdread shared objects that
# player/build_mac.sh already builds/uses. It does NOT rebuild FFmpeg.
#

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$ROOT/dist"
DOWNLOADS="$HOME/Downloads"

CROSS="arm-unknown-linux-gnueabihf-"
CC="${CROSS}gcc"
OBJDUMP="${CROSS}objdump"

COMMON_FLAGS="-march=armv7-a -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard"

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

if [ ! -f "$FF_PREFIX/lib/libswresample.a" ]; then
    echo "ERROR: libswresample.a is not in the current FFmpeg prefix:"
    echo "  $FF_PREFIX/lib"
    echo "Not rebuilding FFmpeg for this experiment. Inspect the prefix first."
    exit 1
fi

for f in "$DVDNAV_SO" "$DVDREAD_SO"; do
    [ -f "$f" ] || {
        echo "ERROR: missing library: $f"
        exit 1
    }
done

mkdir -p "$OUT"
rm -f "$OUT/dvd_audio_test"

echo "Building dvd_audio_test..."

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
    -o "$OUT/dvd_audio_test" \
    "$HERE/dvd_audio_test.c" \
    "$DVDNAV_SO" \
    "$DVDREAD_SO" \
    -Wl,--start-group \
        "$FF_PREFIX/lib/libavformat.a" \
        "$FF_PREFIX/lib/libavcodec.a" \
        "$FF_PREFIX/lib/libswresample.a" \
        "$FF_PREFIX/lib/libavutil.a" \
    -Wl,--end-group \
    -Wl,-rpath,'$ORIGIN/../lib' \
    -Wl,-no-pie \
    -pthread \
    -lm \
    -ldl \
    -latomic

echo "Checking binary for VFPv4 fused instructions..."

if "$OBJDUMP" -d "$OUT/dvd_audio_test" \
    | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' >/dev/null
then
    echo "ERROR: VFPv4 instruction found in binary! Refusing to deploy."
    exit 1
fi

echo "PASS: no VFPv4 fused instructions found."
chmod +x "$OUT/dvd_audio_test"

echo
echo "Built: $OUT/dvd_audio_test"
echo "Compiler: $($CC --version | head -1)"
