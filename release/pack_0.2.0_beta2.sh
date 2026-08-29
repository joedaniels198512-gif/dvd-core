#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Package-only v0.2.0-beta.2 from the frozen v0.2.0-beta.1 zip.
# Does not rebuild player, Main, supervisor, or FPGA.
# Does not modify the beta.1 zip or tag.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="0.2.0-beta.2"
BETA1_ZIP="$ROOT/release/MiSTer_DVD_Player_0.2.0-beta.1.zip"
BETA1_SHA_EXPECT="e2fb49a14debc5c545f6813324e72af6e0a5b7df69a6d427254e2a8e2073efde"

RBF_SHA_EXPECT="bb4b32252b253df15acabf8c297883a5f8e6ffb6dee156bc1b95d82a1fc3d1ac"
MAIN_SHA_EXPECT="a2f1d757c3fd8c3de906cc8c060bf2806fe0b6a1b0fca4b0c03b9dc3779699da"
SUP_SHA_EXPECT="4bf081fe210481c1579cce8868f58dca3914aff3ad08f5bc9cad276dd0c30f35"
PLAYER_SHA_EXPECT="0edb459eb255336e9d8a4c3e4979fef4de4d1e89dbd64f5d3200a196c4e1f00c"

STAGE="$ROOT/release/MiSTer_DVD_Player_${VER}"
SRC_STAGE="$ROOT/release/MiSTer_DVD_Player_${VER}-source"
ZIP="$ROOT/release/MiSTer_DVD_Player_${VER}.zip"
SRC_ZIP="$ROOT/release/MiSTer_DVD_Player_${VER}-source.zip"
SHAFILE="$ROOT/release/MiSTer_DVD_Player_${VER}.sha256"

sh256() { shasum -a 256 "$1" | awk '{print $1}'; }

[ -f "$BETA1_ZIP" ] || { echo "ERROR: missing frozen beta.1 zip" >&2; exit 1; }
[ "$(sh256 "$BETA1_ZIP")" = "$BETA1_SHA_EXPECT" ] \
    || { echo "ERROR: beta.1 zip hash changed; refusing to pack" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
unzip -q "$BETA1_ZIP" -d "$TMP/b1"

echo "Staging install tree $STAGE"
rm -rf "$STAGE" "$SRC_STAGE"
mkdir -p \
    "$STAGE/DVD/bin" \
    "$STAGE/DVD/lib" \
    "$STAGE/DVD/logs" \
    "$STAGE/DVD/config" \
    "$STAGE/games/DVD-Player" \
    "$STAGE/LICENSES" \
    "$SRC_STAGE/LICENSES" \
    "$SRC_STAGE/SOURCES/debian-libdvdread" \
    "$SRC_STAGE/SOURCES/debian-libdvdnav" \
    "$SRC_STAGE/SOURCES/mister-dvd-player-arm/main-dvd/appliance"

# Runtime binaries: byte-copy from frozen beta.1 only.
cp -f "$TMP/b1/DVD_Player.rbf" "$STAGE/DVD_Player.rbf"
cp -f "$TMP/b1/MiSTer_DVD" "$STAGE/MiSTer_DVD"
cp -f "$TMP/b1/DVD/bin/dvd_player" "$STAGE/DVD/bin/dvd_player"
cp -f "$TMP/b1/DVD/bin/dvd_av_threaded_test" "$STAGE/DVD/bin/dvd_av_threaded_test"
cp -f "$TMP/b1/DVD/lib/libdvdnav.so.4.3.0" "$STAGE/DVD/lib/libdvdnav.so.4.3.0"
cp -f "$TMP/b1/DVD/lib/libdvdnav.so.4" "$STAGE/DVD/lib/libdvdnav.so.4"
cp -f "$TMP/b1/DVD/lib/libdvdread.so.8.0.0" "$STAGE/DVD/lib/libdvdread.so.8.0.0"
cp -f "$TMP/b1/DVD/lib/libdvdread.so.8" "$STAGE/DVD/lib/libdvdread.so.8"
chmod +x "$STAGE/MiSTer_DVD" "$STAGE/DVD/bin/dvd_player" "$STAGE/DVD/bin/dvd_av_threaded_test"

: > "$STAGE/DVD/logs/.keep"
: > "$STAGE/DVD/config/.keep"
: > "$STAGE/games/DVD-Player/.keep"
echo "$VER" > "$STAGE/DVD/VERSION"

printf '%s\n' \
    "; DVD Player v0.2.0-beta.2 — add this block; do not replace MiSTer.ini" \
    "[DVD-Player]" \
    "main=MiSTer_DVD" \
    > "$STAGE/MiSTer.ini.fragment"

cp -f "$ROOT/release/pkg_docs/README_INSTALL_0.2.0-beta.2.txt" "$STAGE/README.txt"
cp -f "$ROOT/release/pkg_docs/SOURCE_OFFER_0.2.0-beta.2.txt" "$STAGE/SOURCE_OFFER.txt"
cp -f "$ROOT/release/pkg_docs/LICENSES_README_0.2.0-beta.2.txt" "$STAGE/LICENSES/README.txt"

cp -f "$ROOT/LICENSES/COPYING.GPLv2" \
      "$ROOT/LICENSES/COPYING.GPLv3" \
      "$ROOT/LICENSES/FPGA-COPYING.GPLv2" \
      "$ROOT/LICENSES/Main_MiSTer-COPYING.GPLv3" \
      "$ROOT/LICENSES/FFmpeg-LICENSE.md" \
      "$ROOT/LICENSES/FFmpeg-COPYING.LGPLv2.1" \
      "$ROOT/LICENSES/libdvdnav.copyright" \
      "$ROOT/LICENSES/libdvdread.copyright" \
      "$STAGE/LICENSES/"

# Hash gate — stop if any runtime binary drifted.
fail_hash() { echo "ERROR: $1 hash changed; STOP" >&2; exit 1; }
[ "$(sh256 "$STAGE/DVD_Player.rbf")" = "$RBF_SHA_EXPECT" ] || fail_hash RBF
[ "$(sh256 "$STAGE/MiSTer_DVD")" = "$MAIN_SHA_EXPECT" ] || fail_hash Main
[ "$(sh256 "$STAGE/DVD/bin/dvd_player")" = "$SUP_SHA_EXPECT" ] || fail_hash supervisor
[ "$(sh256 "$STAGE/DVD/bin/dvd_av_threaded_test")" = "$PLAYER_SHA_EXPECT" ] || fail_hash player

# Companion corresponding-source archive (not extracted onto SD).
cp -f "$ROOT/LICENSING.md" "$SRC_STAGE/"
cp -f "$ROOT/THIRD_PARTY_NOTICES.md" "$SRC_STAGE/"
cp -f "$ROOT/SOURCE_INFO.md" "$SRC_STAGE/"
cp -f "$ROOT/LEGAL.md" "$SRC_STAGE/"
cp -f "$ROOT/release/pkg_docs/SOURCE_OFFER_0.2.0-beta.2.txt" "$SRC_STAGE/SOURCE_OFFER.txt"
cp -f "$ROOT/LICENSES/COPYING.GPLv2" \
      "$ROOT/LICENSES/COPYING.GPLv3" \
      "$ROOT/LICENSES/FPGA-COPYING.GPLv2" \
      "$ROOT/LICENSES/Main_MiSTer-COPYING.GPLv3" \
      "$ROOT/LICENSES/FFmpeg-LICENSE.md" \
      "$ROOT/LICENSES/FFmpeg-COPYING.LGPLv2.1" \
      "$ROOT/LICENSES/libdvdnav.copyright" \
      "$ROOT/LICENSES/libdvdread.copyright" \
      "$SRC_STAGE/LICENSES/"
cp -f "$TMP/b1/SOURCES/SOURCE_INFO.md" "$SRC_STAGE/SOURCES/" 2>/dev/null || true
cp -f "$TMP/b1/SOURCES/CORRESPONDING_SOURCE.md" "$SRC_STAGE/SOURCES/" 2>/dev/null || true
cp -f "$TMP/b1/SOURCES/debian-libdvdread/"* "$SRC_STAGE/SOURCES/debian-libdvdread/"
cp -f "$TMP/b1/SOURCES/debian-libdvdnav/"* "$SRC_STAGE/SOURCES/debian-libdvdnav/"
cp -f "$TMP/b1/SOURCES/ffmpeg-6.0.1.tar.xz" "$SRC_STAGE/SOURCES/"
cp -R "$TMP/b1/SOURCES/mister-dvd-player-arm/." "$SRC_STAGE/SOURCES/mister-dvd-player-arm/"

"$ROOT/release/verify_no_libdvdcss.sh" "$STAGE"
"$ROOT/release/verify_no_libdvdcss.sh" "$SRC_STAGE"

if find "$STAGE" -iname '*dvdcss*' | grep . >/dev/null; then
    echo "ERROR: css-related filename in install stage" >&2
    exit 1
fi
if grep -R "DVD_Appliance\|DVD-Player-Appliance\|MiSTer_DVD_Appliance" "$STAGE" \
        --include='*.txt' --include='*.md' --include='*.fragment' >/dev/null 2>&1; then
    echo "ERROR: Appliance path leaked into install zip" >&2
    exit 1
fi

rm -f "$ZIP" "$SRC_ZIP"
( cd "$STAGE" && zip -r -X "$ZIP" . -x "*.DS_Store" -x "*__MACOSX*" )
( cd "$SRC_STAGE" && zip -r -X "$SRC_ZIP" . -x "*.DS_Store" -x "*__MACOSX*" )

if unzip -Z1 "$ZIP" | grep -E '(^|/)libdvdcss(\.so|$)|libdvdcss\.so\.2'; then
    echo "ERROR: install zip contains libdvdcss" >&2
    exit 1
fi
if unzip -Z1 "$ZIP" | grep -v '^SOURCES/' | grep -Ei 'Appliance|dvd_launcher|test-builds'; then
    echo "ERROR: install zip contains banned names" >&2
    exit 1
fi

# Frozen beta.1 must still be the same file after packing.
[ "$(sh256 "$BETA1_ZIP")" = "$BETA1_SHA_EXPECT" ] \
    || { echo "ERROR: beta.1 zip mutated during pack" >&2; exit 1; }

{
    echo "$(sh256 "$ZIP")  $ZIP"
    echo "$(sh256 "$SRC_ZIP")  $SRC_ZIP"
} | tee "$SHAFILE"

echo "Install zip: $ZIP"
echo "Source zip:  $SRC_ZIP"
