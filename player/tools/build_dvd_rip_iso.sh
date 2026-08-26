#!/bin/bash
set -euo pipefail

#
# Build dvd_rip_iso for the SS1 and stage the ARM rip toolchain:
#   dvdbackup 0.4.2 (Debian armhf, dynamic libdvdread)
#   genisoimage 1.1.11 (Debian armhf cdrkit, -dvd-video)
#   libmagic / zlib / bzip2 for genisoimage
#
# Same Cortex-A9 / NEON / VFPv3 toolchain as player/build_mac.sh.
# Does NOT compile FPGA. Does NOT rebuild FFmpeg.
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
DVDBACKUP_BIN="$DOWNLOADS/dvdbackup-extracted/usr/bin/dvdbackup"
GENISO_DIR="$DOWNLOADS/genisoimage-extracted"

for t in "$CC" "$OBJDUMP"; do
    command -v "$t" >/dev/null || { echo "Missing tool: $t"; exit 1; }
done

[ -f "$DVDREAD_SO" ] || { echo "ERROR: missing $DVDREAD_SO"; exit 1; }
[ -f "$DVDBACKUP_BIN" ] || { echo "ERROR: missing $DVDBACKUP_BIN"; exit 1; }
[ -x "$GENISO_DIR/usr/bin/genisoimage" ] || {
    echo "ERROR: missing genisoimage at $GENISO_DIR/usr/bin/genisoimage"
    exit 1
}

mkdir -p "$OUT/rip-lib"
rm -f "$OUT/dvd_rip_iso"

echo "Building dvd_rip_iso..."

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
    -o "$OUT/dvd_rip_iso" \
    "$HERE/dvd_rip_iso.c" \
    "$HERE/dvd_library.c" \
    "$DVDREAD_SO" \
    -Wl,-rpath,'$ORIGIN/../lib:$ORIGIN' \
    -Wl,-no-pie

echo "Checking binary for VFPv4 fused instructions..."

if "$OBJDUMP" -d "$OUT/dvd_rip_iso" \
    | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' >/dev/null
then
    echo "ERROR: VFPv4 instruction found in binary! Refusing to deploy."
    exit 1
fi

echo "PASS: no VFPv4 fused instructions found."
chmod +x "$OUT/dvd_rip_iso"

cp "$DVDBACKUP_BIN" "$OUT/dvdbackup"
chmod +x "$OUT/dvdbackup"
cp "$GENISO_DIR/usr/bin/genisoimage" "$OUT/genisoimage"
chmod +x "$OUT/genisoimage"
cp -a "$GENISO_DIR/rip-lib/." "$OUT/rip-lib/"
cp "$GENISO_DIR/share/magic.mgc" "$OUT/magic.mgc"

echo
echo "Built: $OUT/dvd_rip_iso"
echo "Staged: dvdbackup genisoimage rip-lib/ magic.mgc"
echo "Compiler: $($CC --version | head -1)"
