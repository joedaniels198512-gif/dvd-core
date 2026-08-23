#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DOWNLOADS="$HOME/Downloads"

# IMPORTANT:
# Generic ARM hard-float toolchain.
# Do NOT use armv7-unknown-linux-gnueabihf here:
# that toolchain's runtime contained VFPv4 instructions.
CROSS="arm-unknown-linux-gnueabihf-"

CC="${CROSS}gcc"
AR="${CROSS}ar"
RANLIB="${CROSS}ranlib"
STRIP="${CROSS}strip"
OBJDUMP="${CROSS}objdump"

DVDNAV_DEV="$DOWNLOADS/dvdnav-dev-extracted/usr/include"
DVDREAD_DEV="$DOWNLOADS/dvdread-dev-extracted/usr/include"

DVDNAV_SO="$DOWNLOADS/dvdnav-lib-extracted/usr/lib/arm-linux-gnueabihf/libdvdnav.so.4.3.0"
DVDREAD_SO="$DOWNLOADS/dvdread-extracted/usr/lib/arm-linux-gnueabihf/libdvdread.so.8.0.0"

FFVER="6.0.1"

BUILD="$ROOT/.build"
FF_TARBALL="$BUILD/ffmpeg-$FFVER.tar.xz"
FF_SRC="$BUILD/ffmpeg-$FFVER"

# New FFmpeg build specifically for the SS1 Cortex-A9 / VFPv3.
FF_PREFIX="$BUILD/ffmpeg-ss1-cortex-a9-video"

OUT="$ROOT/dist"

COMMON_FLAGS="-march=armv7-a -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard"

for t in "$CC" "$AR" "$RANLIB" "$STRIP" "$OBJDUMP" curl tar make; do
    command -v "$t" >/dev/null || {
        echo "Missing tool: $t"
        exit 1
    }
done

mkdir -p "$BUILD" "$OUT"

#
# Reuse an FFmpeg source tarball we already downloaded.
#
if [ ! -f "$FF_TARBALL" ]; then

    for OLD in \
        "$DOWNLOADS/dvdplayer-native-v0.8-diagnostics/.build/ffmpeg-$FFVER.tar.xz" \
        "$DOWNLOADS/dvdplayer-native-v0.4/.build/ffmpeg-$FFVER.tar.xz"
    do
        if [ -f "$OLD" ]; then
            echo "Reusing FFmpeg source tarball..."
            cp "$OLD" "$FF_TARBALL"
            break
        fi
    done
fi

if [ ! -f "$FF_TARBALL" ]; then
    echo "Downloading FFmpeg $FFVER..."
    curl -L --fail \
        "https://ffmpeg.org/releases/ffmpeg-$FFVER.tar.xz" \
        -o "$FF_TARBALL"
fi

#
# Build FFmpeg only once.
#
if [ ! -f "$FF_PREFIX/lib/libavformat.a" ]; then

    echo
    echo "================================================"
    echo "Building FFmpeg for SS1 Cortex-A9 / VFPv3"
    echo "Compiler: $CC"
    echo "NO VFPv4"
    echo "Cortex-A9 ARM/NEON optimisations enabled"
    echo "================================================"
    echo

    rm -rf "$FF_SRC" "$FF_PREFIX"

    tar -C "$BUILD" -xf "$FF_TARBALL"

    cd "$FF_SRC"

    ./configure \
        --prefix="$FF_PREFIX" \
        --target-os=linux \
        --arch=arm \
        --cpu=cortex-a9 \
        --enable-cross-compile \
        --cross-prefix="$CROSS" \
        --cc="$CC" \
        --ar="$AR" \
        --ranlib="$RANLIB" \
        --strip="$STRIP" \
        --disable-programs \
        --disable-doc \
        --disable-debug \
        --disable-network \
        --disable-autodetect \
        --disable-everything \
        --enable-avcodec \
        --enable-avformat \
        --enable-avutil \
        --enable-swscale \
        --enable-demuxer=mpegps \
        --enable-decoder=mpeg1video \
        --enable-decoder=mpeg2video \
        --enable-decoder=ac3 \
        --enable-decoder=eac3 \
        --enable-decoder=mp1 \
        --enable-decoder=mp2 \
        --enable-decoder=mp3 \
        --enable-decoder=dca \
        --enable-decoder=pcm_dvd \
        --enable-parser=mpegvideo \
        --enable-parser=ac3 \
        --enable-parser=mpegaudio \
        --enable-parser=dca \
        --enable-pthreads \
        --extra-cflags="-O2 $COMMON_FLAGS" \
        --extra-ldflags="-pthread"

    if ! grep -q '^#define CONFIG_MPEGPS_DEMUXER 1' config_components.h; then
        echo "ERROR: MPEG-PS demuxer was not enabled."
        exit 1
    fi

    make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    make install

else

    echo "Reusing SS1 VFPv3-safe FFmpeg libraries."

fi

cd "$ROOT"

rm -f "$OUT/dvdplayer_headless"

echo
echo "Building DVD diagnostic..."

"$CC" \
    -O2 \
    -g3 \
    -fno-omit-frame-pointer \
    -std=gnu11 \
    $COMMON_FLAGS \
    -I"$FF_PREFIX/include" \
    -I"$DVDNAV_DEV" \
    -I"$DVDREAD_DEV" \
    -o "$OUT/dvdplayer_headless" \
    "$ROOT/src/dvdplayer_headless.c" \
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
# Critical safety check.
#
# VFMA / VFMS / VFNMA / VFNMS are VFPv4 instructions.
# The SS1 Cortex-A9 only has VFPv3.
#
echo
echo "Checking final binary for VFPv4 fused instructions..."

if "$OBJDUMP" -d "$OUT/dvdplayer_headless" \
    | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' >/dev/null
then
    echo
    echo "ERROR: VFPv4 instruction found in binary!"
    echo "Refusing to deploy."
    exit 1
fi

echo "PASS: no VFPv4 fused instructions found."

cp "$ROOT/scripts/Headless_Test.sh" "$OUT/Headless_Test.sh"

chmod +x \
    "$OUT/dvdplayer_headless" \
    "$OUT/Headless_Test.sh"

echo
echo "============================================"
echo "Build complete."
echo "Compiler: $($CC --version | head -1)"
echo "Target: ARMv7-A / VFPv3 / hard-float"
echo "============================================"
