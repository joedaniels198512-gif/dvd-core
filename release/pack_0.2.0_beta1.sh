#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Stage / zip public v0.2.0-beta.1. Extract to MiSTer SD root.
# No Quartus. No libdvdcss. No old launcher. No installer.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="0.2.0-beta.1"
STAGE="$ROOT/release/MiSTer_DVD_Player_${VER}"
ZIP="$ROOT/release/MiSTer_DVD_Player_${VER}.zip"
SHAFILE="$ROOT/release/MiSTer_DVD_Player_${VER}.sha256"
DIST="$ROOT/player/dist"
CS="$ROOT/release/corresponding-source"
FF_TARBALL="$ROOT/player/.build/ffmpeg-6.0.1.tar.xz"
[ -f "$FF_TARBALL" ] || FF_TARBALL="$ROOT/release/src-cache/ffmpeg-6.0.1.tar.xz"

RBF_SHA_EXPECT="bb4b32252b253df15acabf8c297883a5f8e6ffb6dee156bc1b95d82a1fc3d1ac"
PLAYER_SHA_EXPECT="0edb459eb255336e9d8a4c3e4979fef4de4d1e89dbd64f5d3200a196c4e1f00c"

RBF_SRC="${PUBLIC_RBF:-$ROOT/release/test-builds/final-aspect-audio-test/DVD_Player.rbf}"

sh256() { shasum -a 256 "$1" | awk '{print $1}'; }

echo "Staging $STAGE"
rm -rf "$STAGE"
mkdir -p \
    "$STAGE/DVD/bin" \
    "$STAGE/DVD/lib" \
    "$STAGE/DVD/logs" \
    "$STAGE/DVD/config" \
    "$STAGE/games/DVD-Player" \
    "$STAGE/LICENSES" \
    "$STAGE/SOURCES/mister-dvd-player-arm/main-dvd/appliance" \
    "$STAGE/SOURCES/debian-libdvdread" \
    "$STAGE/SOURCES/debian-libdvdnav"

[ -f "$RBF_SRC" ] || { echo "ERROR: RBF not found: $RBF_SRC (set PUBLIC_RBF)" >&2; exit 1; }
[ "$(sh256 "$RBF_SRC")" = "$RBF_SHA_EXPECT" ] || { echo "ERROR: RBF hash mismatch"; exit 1; }

MAIN_BIN="$ROOT/main-dvd/bin/MiSTer_DVD"
[ -f "$MAIN_BIN" ] || { echo "ERROR: missing $MAIN_BIN"; exit 1; }
[ -f "$DIST/dvd_player" ] || { echo "ERROR: missing $DIST/dvd_player"; exit 1; }
[ -f "$DIST/dvd_av_threaded_test" ] || { echo "ERROR: missing player"; exit 1; }
[ "$(sh256 "$DIST/dvd_av_threaded_test")" = "$PLAYER_SHA_EXPECT" ] \
    || { echo "ERROR: player hash mismatch"; exit 1; }

cp -f "$RBF_SRC" "$STAGE/DVD_Player.rbf"
cp -f "$MAIN_BIN" "$STAGE/MiSTer_DVD"
cp -f "$DIST/dvd_player" "$STAGE/DVD/bin/"
cp -f "$DIST/dvd_av_threaded_test" "$STAGE/DVD/bin/"

cp -f "$ROOT/release/test-builds/aspect-map-fix/../final-aspect-audio-test/lib/libdvdnav.so.4.3.0" \
      "$STAGE/DVD/lib/" 2>/dev/null || \
cp -f "$HOME/Downloads/dvdnav-lib-extracted/usr/lib/arm-linux-gnueabihf/libdvdnav.so.4.3.0" \
      "$STAGE/DVD/lib/"
cp -f "$STAGE/DVD/lib/libdvdnav.so.4.3.0" "$STAGE/DVD/lib/libdvdnav.so.4"
cp -f "$HOME/Downloads/dvdread-extracted/usr/lib/arm-linux-gnueabihf/libdvdread.so.8.0.0" \
      "$STAGE/DVD/lib/"
cp -f "$STAGE/DVD/lib/libdvdread.so.8.0.0" "$STAGE/DVD/lib/libdvdread.so.8"

chmod +x "$STAGE/MiSTer_DVD" \
         "$STAGE/DVD/bin/dvd_player" \
         "$STAGE/DVD/bin/dvd_av_threaded_test"

: > "$STAGE/DVD/logs/.keep"
: > "$STAGE/DVD/config/.keep"
: > "$STAGE/games/DVD-Player/.keep"
echo "$VER" > "$STAGE/DVD/VERSION"

printf '%s\n' \
    "; DVD Player v0.2.0-beta.1 — add this block; do not replace MiSTer.ini" \
    "[DVD-Player]" \
    "main=MiSTer_DVD" \
    > "$STAGE/MiSTer.ini.fragment"

cp -f "$ROOT/LICENSES/COPYING.GPLv2" \
      "$ROOT/LICENSES/COPYING.GPLv3" \
      "$ROOT/LICENSES/FPGA-COPYING.GPLv2" \
      "$ROOT/LICENSES/Main_MiSTer-COPYING.GPLv3" \
      "$ROOT/LICENSES/FFmpeg-LICENSE.md" \
      "$ROOT/LICENSES/FFmpeg-COPYING.LGPLv2.1" \
      "$ROOT/LICENSES/libdvdnav.copyright" \
      "$ROOT/LICENSES/libdvdread.copyright" \
      "$ROOT/LICENSES/README.md" \
      "$STAGE/LICENSES/"
cp -f "$ROOT/LICENSING.md" "$STAGE/"
cp -f "$ROOT/SOURCE_INFO.md" "$STAGE/SOURCES/"
cp -f "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/"
cp -f "$ROOT/README.md" "$STAGE/"
cp -f "$ROOT/INSTALL.md" "$STAGE/"
cp -f "$ROOT/LEGAL.md" "$STAGE/"
cp -f "$ROOT/PRIVACY.md" "$STAGE/"
cp -f "$ROOT/CREDITS.md" "$STAGE/"
cp -f "$ROOT/CHANGELOG.md" "$STAGE/"
cp -f "$ROOT/KNOWN_ISSUES.md" "$STAGE/"
cp -f "$ROOT/COMPATIBILITY.md" "$STAGE/"
cp -f "$ROOT/CONTRIBUTING.md" "$STAGE/"

if [ -d "$CS" ]; then
    cp -f "$CS/libdvdread_6.1.1-2.dsc" \
          "$CS/libdvdread_6.1.1.orig.tar.bz2" \
          "$CS/libdvdread_6.1.1-2.debian.tar.xz" \
          "$STAGE/SOURCES/debian-libdvdread/" 2>/dev/null || true
    cp -f "$CS/libdvdnav_6.1.0-1.dsc" \
          "$CS/libdvdnav_6.1.0.orig.tar.bz2" \
          "$CS/libdvdnav_6.1.0-1.debian.tar.xz" \
          "$STAGE/SOURCES/debian-libdvdnav/" 2>/dev/null || true
    [ -f "$CS/README.md" ] && cp -f "$CS/README.md" "$STAGE/SOURCES/CORRESPONDING_SOURCE.md"
fi
if [ -f "$FF_TARBALL" ]; then
    cp -f "$FF_TARBALL" "$STAGE/SOURCES/ffmpeg-6.0.1.tar.xz"
fi

ARM="$STAGE/SOURCES/mister-dvd-player-arm"
cp -f "$ROOT/player/tools/dvd_av_threaded_test.c" \
      "$ROOT/player/tools/dvd_appliance.c" \
      "$ROOT/player/tools/build_dvd_av_threaded_test.sh" \
      "$ROOT/player/tools/build_dvd_appliance.sh" \
      "$ARM/"
cp -f "$ROOT/main-dvd/appliance/dvd_main.cpp" \
      "$ROOT/main-dvd/appliance/dvd_main.h" \
      "$ROOT/main-dvd/appliance/apply_iso_hooks.py" \
      "$ARM/main-dvd/appliance/"
cp -f "$ROOT/main-dvd/apply_dvd_hooks.py" \
      "$ROOT/main-dvd/build_mister_dvd_appliance.sh" \
      "$ROOT/main-dvd/UPSTREAM_COMMIT" \
      "$ARM/main-dvd/"

python3 - <<PY
from pathlib import Path
root = Path("$STAGE")
for p in root.rglob("*"):
    if p.is_symlink():
        target = p.resolve()
        p.unlink()
        p.write_bytes(target.read_bytes())
print("symlinks flattened")
PY

[ "$(sh256 "$STAGE/DVD_Player.rbf")" = "$RBF_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/bin/dvd_av_threaded_test")" = "$PLAYER_SHA_EXPECT" ]

"$ROOT/release/verify_no_libdvdcss.sh" "$STAGE"

if find "$STAGE" -iname '*dvdcss*' | grep . >/dev/null; then
    echo "ERROR: css-related file in stage" >&2
    exit 1
fi
if find "$STAGE" \( -iname '*dvd_launcher*' -o -iname 'MiSTer_DVD_Appliance' \
        -o -iname 'DVD_Player_Appliance.rbf' -o -iname 'DVD_Appliance' \) \
        ! -path '*/SOURCES/*' | grep . >/dev/null; then
    echo "ERROR: launcher or Appliance artifact staged" >&2
    find "$STAGE" \( -iname '*dvd_launcher*' -o -iname '*Appliance*' \) ! -path '*/SOURCES/*'
    exit 1
fi
if grep -R "DVD_Appliance\|DVD-Player-Appliance" "$STAGE" \
        --include='*.md' --include='*.txt' --include='*.fragment' \
        --exclude-dir=SOURCES >/dev/null 2>&1; then
    echo "ERROR: Appliance path leaked into user-facing package docs" >&2
    grep -R "DVD_Appliance\|DVD-Player-Appliance" "$STAGE" \
        --include='*.md' --include='*.txt' --exclude-dir=SOURCES || true
    exit 1
fi

for required in \
    "$STAGE/DVD_Player.rbf" \
    "$STAGE/MiSTer_DVD" \
    "$STAGE/DVD/bin/dvd_player" \
    "$STAGE/DVD/bin/dvd_av_threaded_test" \
    "$STAGE/DVD/lib/libdvdnav.so.4.3.0" \
    "$STAGE/DVD/lib/libdvdread.so.8.0.0" \
    "$STAGE/games/DVD-Player/.keep" \
    "$STAGE/README.md" \
    "$STAGE/INSTALL.md" \
    "$STAGE/KNOWN_ISSUES.md" \
    "$STAGE/LICENSING.md" \
    "$STAGE/THIRD_PARTY_NOTICES.md"
do
    [ -f "$required" ] || { echo "ERROR: missing $required" >&2; exit 1; }
done

(
    cd "$STAGE"
    find . -type f ! -name 'MiSTer_DVD_Player_CHECKSUMS.txt' | LC_ALL=C sort | while read -r f; do
        shasum -a 256 "$f"
    done
) > "$STAGE/MiSTer_DVD_Player_CHECKSUMS.txt"

rm -f "$ZIP"
( cd "$STAGE" && zip -r -X "$ZIP" . -x "*.DS_Store" -x "*__MACOSX*" )
"$ROOT/release/verify_no_libdvdcss.sh" "$STAGE"
if unzip -Z1 "$ZIP" | grep -E '(^|/)libdvdcss\.so(\.2(\.2\.0)?)?$'; then
    echo "ERROR: zip contains a libdvdcss binary" >&2
    exit 1
fi
if unzip -Z1 "$ZIP" | grep -v '^SOURCES/' | grep -E '(^|/)libdvdcss(\.so|$)|Appliance|dvd_launcher'; then
    echo "ERROR: zip contains banned names" >&2
    exit 1
fi
sh256 "$ZIP" | awk -v z="$ZIP" '{print $1 "  " z}' | tee "$SHAFILE"
echo "Staged $STAGE"
