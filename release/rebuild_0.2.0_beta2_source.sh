#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Rebuild ONLY the v0.2.0-beta.2 companion source zip.
# Does not touch the frozen install zip or any runtime binary.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL_ZIP="$ROOT/release/MiSTer_DVD_Player_0.2.0-beta.2.zip"
INSTALL_SHA_EXPECT="1e04a2c8807699ce89f52171703c0291d7ee682eda9966eec8095cf2be86872a"
OLD_SRC_ZIP="$ROOT/release/MiSTer_DVD_Player_0.2.0-beta.2-source.zip"
SRC_STAGE="$ROOT/release/MiSTer_DVD_Player_0.2.0-beta.2-source"
SRC_ZIP="$ROOT/release/MiSTer_DVD_Player_0.2.0-beta.2-source.zip"

RBF_SHA_EXPECT="bb4b32252b253df15acabf8c297883a5f8e6ffb6dee156bc1b95d82a1fc3d1ac"
MAIN_SHA_EXPECT="a2f1d757c3fd8c3de906cc8c060bf2806fe0b6a1b0fca4b0c03b9dc3779699da"
SUP_SHA_EXPECT="4bf081fe210481c1579cce8868f58dca3914aff3ad08f5bc9cad276dd0c30f35"
PLAYER_SHA_EXPECT="0edb459eb255336e9d8a4c3e4979fef4de4d1e89dbd64f5d3200a196c4e1f00c"

sh256() { shasum -a 256 "$1" | awk '{print $1}'; }

[ -f "$INSTALL_ZIP" ] || { echo "ERROR: missing frozen install zip" >&2; exit 1; }
[ "$(sh256 "$INSTALL_ZIP")" = "$INSTALL_SHA_EXPECT" ] \
    || { echo "ERROR: install zip hash changed; STOP" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
unzip -q "$INSTALL_ZIP" -d "$TMP/install"
[ "$(sh256 "$TMP/install/DVD_Player.rbf")" = "$RBF_SHA_EXPECT" ]
[ "$(sh256 "$TMP/install/MiSTer_DVD")" = "$MAIN_SHA_EXPECT" ]
[ "$(sh256 "$TMP/install/DVD/bin/dvd_player")" = "$SUP_SHA_EXPECT" ]
[ "$(sh256 "$TMP/install/DVD/bin/dvd_av_threaded_test")" = "$PLAYER_SHA_EXPECT" ]

# Restage source tree from the previous source zip if present, else from beta.1.
rm -rf "$SRC_STAGE"
mkdir -p "$SRC_STAGE"
if [ -f "$OLD_SRC_ZIP" ]; then
    unzip -q "$OLD_SRC_ZIP" -d "$SRC_STAGE"
else
    echo "ERROR: previous source zip missing" >&2
    exit 1
fi

FPGA="$SRC_STAGE/SOURCES/mister-dvd-player-fpga"
rm -rf "$FPGA"
mkdir -p "$FPGA"

# Copy Quartus project inputs only. Do not modify fpga/ in the repo.
rsync -a \
    --exclude 'db/' \
    --exclude 'incremental_db/' \
    --exclude 'output_files/' \
    --exclude 'sim/' \
    --exclude 'greybox_tmp/' \
    --exclude '.DS_Store' \
    --exclude '*.srf' \
    --exclude 'Template.qpf' \
    --exclude 'Template.qsf' \
    --exclude 'Template.sdc' \
    --exclude 'Template.sv' \
    --exclude 'Template.srf' \
    --exclude 'Template_Q13.qpf' \
    --exclude 'Template_Q13.qsf' \
    --exclude 'Template_Q13.srf' \
    --exclude 'clean.bat' \
    --exclude 'jtag.cdf' \
    "$ROOT/fpga/" "$FPGA/"

cp -f "$ROOT/release/pkg_docs/FPGA_BUILD_0.2.0-beta.2.txt" "$FPGA/BUILD.txt"
cp -f "$ROOT/.github/workflows/build-core.yml" "$FPGA/BUILD-GHA.yml"
cp -f "$ROOT/release/pkg_docs/CORRESPONDING_SOURCE_0.2.0-beta.2.md" \
      "$SRC_STAGE/SOURCES/CORRESPONDING_SOURCE.md"
cp -f "$ROOT/release/pkg_docs/CORRESPONDING_SOURCE_0.2.0-beta.2.md" \
      "$SRC_STAGE/CORRESPONDING_SOURCE.md"

# Point SOURCE_OFFER at the FPGA tree now included here.
cat > "$SRC_STAGE/SOURCE_OFFER.txt" <<'EOF'
Corresponding source offer — MiSTer DVD Player v0.2.0-beta.2

This archive accompanies MiSTer_DVD_Player_0.2.0-beta.2.zip.

See SOURCES/CORRESPONDING_SOURCE.md for the binary → source map.

FPGA/HDL for DVD_Player.rbf is in SOURCES/mister-dvd-player-fpga/.
ARM player, supervisor, and Main overlay are in SOURCES/mister-dvd-player-arm/.
Debian libdvdnav / libdvdread triples and FFmpeg 6.0.1 are under SOURCES/.

libdvdcss is not distributed.
EOF

"$ROOT/release/verify_no_libdvdcss.sh" "$SRC_STAGE"

# Refuse Quartus junk in the staged FPGA tree.
if [ -d "$FPGA/db" ] || [ -d "$FPGA/incremental_db" ] || [ -d "$FPGA/output_files" ]; then
    echo "ERROR: Quartus cache/output leaked into FPGA source tree" >&2
    exit 1
fi
[ -f "$FPGA/DVD.sv" ] && [ -f "$FPGA/DVD.qpf" ] && [ -f "$FPGA/DVD.qsf" ] \
    && [ -f "$FPGA/sys/sys_top.v" ] && [ -f "$FPGA/rtl/yuv601_rgb.v" ] \
    || { echo "ERROR: FPGA source tree incomplete" >&2; exit 1; }

rm -f "$SRC_ZIP"
( cd "$SRC_STAGE" && zip -r -X "$SRC_ZIP" . -x "*.DS_Store" -x "*__MACOSX*" )

if unzip -Z1 "$SRC_ZIP" | grep -E '(^|/)libdvdcss\.so(\.2(\.2\.0)?)?$'; then
    echo "ERROR: source zip contains libdvdcss binary" >&2
    exit 1
fi
if unzip -Z1 "$SRC_ZIP" | grep -E '(^|/)(db|incremental_db|output_files)/'; then
    echo "ERROR: Quartus junk in source zip" >&2
    exit 1
fi

# Install zip must still be byte-identical.
[ "$(sh256 "$INSTALL_ZIP")" = "$INSTALL_SHA_EXPECT" ] \
    || { echo "ERROR: install zip mutated" >&2; exit 1; }

echo "INSTALL still $(sh256 "$INSTALL_ZIP")"
echo "SOURCE now    $(sh256 "$SRC_ZIP")  $SRC_ZIP"
