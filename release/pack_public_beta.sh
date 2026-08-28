#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Stage / zip PUBLIC beta 0.1.
# Does not deploy, does not run Quartus, does not rebuild the player.
# Ships the working Rip DVD stack. Does NOT copy or fetch libdvdcss.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="0.1.0-public-beta"
STAGE="$ROOT/release/MiSTer_DVD_Player_${VER}"
ZIP="$ROOT/release/MiSTer_DVD_Player_${VER}.zip"
SHAFILE="$ROOT/release/MiSTer_DVD_Player_${VER}.sha256"
DIST="$ROOT/player/dist"
DL="$HOME/Downloads"
CS="$ROOT/release/corresponding-source"
FF_TARBALL="$ROOT/player/.build/ffmpeg-6.0.1.tar.xz"
[ -f "$FF_TARBALL" ] || FF_TARBALL="$ROOT/release/src-cache/ffmpeg-6.0.1.tar.xz"

RBF_SHA_EXPECT="3d841dc03aacfd58e21de8a7f0721d45bbfbf0c5d0b48bed760fa270c74c24f5"
PLAYER_SHA_EXPECT="89ed3327df140d98fc5e2c7edcf6312d798f8ed50ba82aa70abe3404be0df09c"
LAUNCHER_SHA_EXPECT="f4fea091a7fdb53200cb5ab9a65c21d051f127ea6fd6e0e6aa0c01f0ce39e58f"
MAIN_SHA_EXPECT="846eb22318bc6fd59c72e766daba6f9b40179b5bd231e358b8a9bfb89b8ade84"
DVDREAD_SHA_EXPECT="222de9bd1f40c01514f8dd88fa30bb669aaae4c09959dbf26dc15061cff534c8"
DVDNAV_SHA_EXPECT="2e113c2e65911713778537947644940278928d2eff3f04390fcafd4aef15980f"
RIP_SHA_EXPECT="42aec76ee04e6eb5b70c962ab764a6eeed897f2876cfc45cb0a39b39df51cecf"
DVDBACKUP_SHA_EXPECT="fbfa1a1f4750a88a59989cd60bc40820fbd61e3b09f29f0738849cb78e6da320"
GENISO_SHA_EXPECT="a114528e04c5262387b358baebc252e31b2be8de4df4e87ca130ee35ac3d7d8e"
MAGIC_SHA_EXPECT="5f7ada0953e39fa859e15eff4ac3f3fc2064cac23769019d6aac7b53cb2c9c54"
LIBMAGIC_SHA_EXPECT="a9693bd66fe1782b5d02043e243dd56e73897d3d4c3a4b17007c4313b9ca1e60"
LIBZ_SHA_EXPECT="e8f5ff20a6dd792bef144d2f3280c62189279f17c0c41db6a32cf7c8534c681a"
LIBBZ2_SHA_EXPECT="6b815fd378d384641e01950b222f86f2dcb163f0ea80afffced4ee5a564d0120"
FFMPEG_TAR_SHA_EXPECT="9b16b8731d78e596b4be0d720428ca42df642bb2d78342881ff7f5bc29fc9623"

RBF_SRC="${PUBLIC_RBF:-/tmp/yuv-stissue-fix-33175603689/rbf/DVD_FPGA_YUV420_DVDPlayer_Main.rbf}"

sh256() { shasum -a 256 "$1" | awk '{print $1}'; }

echo "Staging public package $STAGE"
rm -rf "$STAGE"
mkdir -p \
    "$STAGE/DVD/dev/rip-lib" \
    "$STAGE/DVD/lib" \
    "$STAGE/DVD/logs" \
    "$STAGE/DVD/cache" \
    "$STAGE/DVD/isos" \
    "$STAGE/Scripts" \
    "$STAGE/LICENSES" \
    "$STAGE/SOURCES/mister-dvd-player-arm/main-dvd" \
    "$STAGE/SOURCES/debian-libdvdread" \
    "$STAGE/SOURCES/debian-libdvdnav" \
    "$STAGE/SOURCES/debian-dvdbackup" \
    "$STAGE/SOURCES/debian-cdrkit" \
    "$STAGE/SOURCES/debian-file" \
    "$STAGE/SOURCES/debian-zlib" \
    "$STAGE/SOURCES/debian-bzip2"

[ -f "$RBF_SRC" ] || { echo "ERROR: RBF not found: $RBF_SRC (set PUBLIC_RBF)" >&2; exit 1; }
[ "$(sh256 "$RBF_SRC")" = "$RBF_SHA_EXPECT" ] || { echo "ERROR: RBF hash mismatch"; exit 1; }
[ -f "$FF_TARBALL" ] || { echo "ERROR: FFmpeg corresponding source missing: $FF_TARBALL" >&2; exit 1; }
[ "$(sh256 "$FF_TARBALL")" = "$FFMPEG_TAR_SHA_EXPECT" ] || { echo "ERROR: FFmpeg tarball hash mismatch"; exit 1; }

cp -f "$RBF_SRC" "$STAGE/MiSTer_DVD_Player.rbf"
cp -f "$DIST/dvd_launcher" "$STAGE/DVD/dev/"
cp -f "$DIST/dvd_av_threaded_test" "$STAGE/DVD/dev/"
cp -f "$DIST/dvd_rip_iso" "$STAGE/DVD/dev/"
cp -f "$DIST/dvdbackup" "$STAGE/DVD/dev/"
cp -f "$DIST/genisoimage" "$STAGE/DVD/dev/"
cp -f "$DIST/magic.mgc" "$STAGE/DVD/dev/"
cp -a "$DIST/rip-lib/." "$STAGE/DVD/dev/rip-lib/"
# Working rip helper also searches /media/fat/DVD/lib (see dvd_rip_iso.c).
cp -a "$DIST/rip-lib/." "$STAGE/DVD/lib/"

MAIN_BIN="$ROOT/main-dvd/bin/MiSTer_DVD"
[ -f "$MAIN_BIN" ] || MAIN_BIN="$ROOT/main-dvd/.src/bin/MiSTer_DVD"
[ -f "$MAIN_BIN" ] || { echo "ERROR: MiSTer_DVD binary missing"; exit 1; }
cp -f "$MAIN_BIN" "$STAGE/MiSTer_DVD"

cp -f "$DL/dvdnav-lib-extracted/usr/lib/arm-linux-gnueabihf/libdvdnav.so.4.3.0" "$STAGE/DVD/lib/"
cp -f "$STAGE/DVD/lib/libdvdnav.so.4.3.0" "$STAGE/DVD/lib/libdvdnav.so.4"
cp -f "$DL/dvdread-extracted/usr/lib/arm-linux-gnueabihf/libdvdread.so.8.0.0" "$STAGE/DVD/lib/"
cp -f "$STAGE/DVD/lib/libdvdread.so.8.0.0" "$STAGE/DVD/lib/libdvdread.so.8"

chmod +x "$STAGE/DVD/dev/dvd_launcher" \
         "$STAGE/DVD/dev/dvd_av_threaded_test" \
         "$STAGE/DVD/dev/dvd_rip_iso" \
         "$STAGE/DVD/dev/dvdbackup" \
         "$STAGE/DVD/dev/genisoimage" \
         "$STAGE/MiSTer_DVD"

: > "$STAGE/DVD/logs/.keep"
: > "$STAGE/DVD/cache/.keep"
: > "$STAGE/DVD/isos/.keep"
echo "$VER" > "$STAGE/DVD/VERSION"

cp -f "$ROOT/release/pkg_scripts/Install_MiSTer_DVD_Player.sh" "$STAGE/Scripts/"
cp -f "$ROOT/release/pkg_scripts/Uninstall_MiSTer_DVD_Player.sh" "$STAGE/Scripts/"
chmod +x "$STAGE/Scripts/"*.sh

cp -f "$ROOT/LICENSES/"* "$STAGE/LICENSES/"
cp -f "$ROOT/LICENSING.md" "$STAGE/"
cp -f "$ROOT/SOURCE_INFO.md" "$STAGE/SOURCES/"
cp -f "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/"
cp -f "$ROOT/COMPLIANCE_PUBLIC_BETA_0.1.md" "$STAGE/"
cp -f "$ROOT/README.md" "$STAGE/"
cp -f "$ROOT/INSTALL.md" "$STAGE/"
cp -f "$ROOT/LEGAL.md" "$STAGE/"
cp -f "$ROOT/PRIVACY.md" "$STAGE/"
cp -f "$ROOT/CREDITS.md" "$STAGE/"
cp -f "$ROOT/CHANGELOG.md" "$STAGE/"
cp -f "$ROOT/KNOWN_ISSUES.md" "$STAGE/"
# Do not copy RELEASE_CANDIDATE_0.1.md: it records the private-beta on-device
# layout including libdvdcss hashes. Public terms are COMPLIANCE / SOURCE_INFO.
# RELEASE_NOTES_0.1.md is the GitHub announcement, not an on-device file.

cp -f "$CS/libdvdread_6.1.1-2.dsc" \
      "$CS/libdvdread_6.1.1.orig.tar.bz2" \
      "$CS/libdvdread_6.1.1-2.debian.tar.xz" \
      "$STAGE/SOURCES/debian-libdvdread/"
cp -a "$CS/libdvdread-6.1.1-2-debian-patches/." \
      "$STAGE/SOURCES/debian-libdvdread/"
cp -f "$CS/libdvdnav_6.1.0-1.dsc" \
      "$CS/libdvdnav_6.1.0.orig.tar.bz2" \
      "$CS/libdvdnav_6.1.0-1.debian.tar.xz" \
      "$STAGE/SOURCES/debian-libdvdnav/"
cp -f "$CS/dvdbackup_0.4.2-4.1.dsc" \
      "$CS/dvdbackup_0.4.2.orig.tar.xz" \
      "$CS/dvdbackup_0.4.2-4.1.debian.tar.xz" \
      "$STAGE/SOURCES/debian-dvdbackup/"
cp -a "$CS/dvdbackup-0.4.2-4.1-debian-patches/." \
      "$STAGE/SOURCES/debian-dvdbackup/"
cp -f "$CS/cdrkit_1.1.11-3.2.dsc" \
      "$CS/cdrkit_1.1.11.orig.tar.gz" \
      "$CS/cdrkit_1.1.11-3.2.debian.tar.xz" \
      "$STAGE/SOURCES/debian-cdrkit/"
cp -f "$CS/file_5.39-3+deb11u1.dsc" \
      "$CS/file_5.39.orig.tar.gz" \
      "$CS/file_5.39-3+deb11u1.debian.tar.xz" \
      "$STAGE/SOURCES/debian-file/"
cp -f "$CS/zlib_1.2.11.dfsg-2+deb11u2.dsc" \
      "$CS/zlib_1.2.11.dfsg.orig.tar.gz" \
      "$CS/zlib_1.2.11.dfsg-2+deb11u2.debian.tar.xz" \
      "$STAGE/SOURCES/debian-zlib/"
cp -f "$CS/bzip2_1.0.8-4.dsc" \
      "$CS/bzip2_1.0.8.orig.tar.gz" \
      "$CS/bzip2_1.0.8-4.debian.tar.bz2" \
      "$STAGE/SOURCES/debian-bzip2/"
cp -f "$CS/README.md" "$STAGE/SOURCES/CORRESPONDING_SOURCE.md"
cp -f "$FF_TARBALL" "$STAGE/SOURCES/ffmpeg-6.0.1.tar.xz"

ARM="$STAGE/SOURCES/mister-dvd-player-arm"
for f in \
    dvd_launcher.c dvd_library.c dvd_library.h dvd_rip_iso.c \
    dvd_av_threaded_test.c \
    build_dvd_launcher.sh build_dvd_rip_iso.sh build_dvd_av_threaded_test.sh
do
    cp -f "$ROOT/player/tools/$f" "$ARM/"
done
cp -f "$ROOT/player/build_mac.sh" "$ARM/"
cp -f "$ROOT/main-dvd/dvd_main.cpp" "$ROOT/main-dvd/dvd_main.h" \
      "$ROOT/main-dvd/apply_dvd_hooks.py" "$ROOT/main-dvd/build_mister_dvd.sh" \
      "$ROOT/main-dvd/UPSTREAM_COMMIT" "$ROOT/main-dvd/UPSTREAM.txt" \
      "$ARM/main-dvd/"

# Flatten any symlinks (FAT)
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

# --- hash gates ---
[ "$(sh256 "$STAGE/MiSTer_DVD_Player.rbf")" = "$RBF_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/dev/dvd_av_threaded_test")" = "$PLAYER_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/dev/dvd_launcher")" = "$LAUNCHER_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/MiSTer_DVD")" = "$MAIN_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/lib/libdvdread.so.8.0.0")" = "$DVDREAD_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/lib/libdvdnav.so.4.3.0")" = "$DVDNAV_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/dev/dvd_rip_iso")" = "$RIP_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/dev/dvdbackup")" = "$DVDBACKUP_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/dev/genisoimage")" = "$GENISO_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/dev/magic.mgc")" = "$MAGIC_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/dev/rip-lib/libmagic.so.1.0.0")" = "$LIBMAGIC_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/dev/rip-lib/libz.so.1.2.11")" = "$LIBZ_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/DVD/dev/rip-lib/libbz2.so.1.0.4")" = "$LIBBZ2_SHA_EXPECT" ]
[ "$(sh256 "$STAGE/SOURCES/ffmpeg-6.0.1.tar.xz")" = "$FFMPEG_TAR_SHA_EXPECT" ]

"$ROOT/release/verify_no_libdvdcss.sh" "$STAGE"
"$ROOT/release/verify_conservative_css_policy.sh"

for banned in libdvdcss.so libdvdcss.so.2 libdvdcss.so.2.2.0; do
    if find "$STAGE" -name "$banned" | grep . >/dev/null; then
        echo "ERROR: banned file in stage: $banned" >&2
        exit 1
    fi
done

for required in \
    "$STAGE/DVD/dev/dvd_rip_iso" \
    "$STAGE/DVD/dev/dvdbackup" \
    "$STAGE/DVD/dev/genisoimage" \
    "$STAGE/DVD/dev/magic.mgc" \
    "$STAGE/LICENSES/dvdbackup.copyright" \
    "$STAGE/LICENSES/genisoimage.copyright" \
    "$STAGE/LICENSES/libmagic.copyright" \
    "$STAGE/LICENSES/zlib.copyright" \
    "$STAGE/LICENSES/bzip2.copyright" \
    "$STAGE/README.md" \
    "$STAGE/INSTALL.md" \
    "$STAGE/LEGAL.md" \
    "$STAGE/PRIVACY.md" \
    "$STAGE/CREDITS.md" \
    "$STAGE/CHANGELOG.md" \
    "$STAGE/KNOWN_ISSUES.md" \
    "$STAGE/LICENSING.md" \
    "$STAGE/THIRD_PARTY_NOTICES.md" \
    "$STAGE/SOURCES/SOURCE_INFO.md"
do
    [ -f "$required" ] || { echo "ERROR: missing $required" >&2; exit 1; }
done

if grep -R "/Users/jarvis" "$STAGE/Scripts" >/dev/null 2>&1; then
    echo "ERROR: host path leaked into package scripts" >&2
    exit 1
fi

(
    cd "$STAGE"
    find . -type f ! -name 'MiSTer_DVD_Player_CHECKSUMS.txt' | LC_ALL=C sort | while read -r f; do
        shasum -a 256 "$f"
    done
) > "$STAGE/MiSTer_DVD_Player_CHECKSUMS.txt"

if [ "${PACK_ZIP:-1}" = "1" ]; then
    rm -f "$ZIP"
    ( cd "$STAGE" && zip -r -X "$ZIP" . -x "*.DS_Store" -x "*__MACOSX*" )
    "$ROOT/release/verify_no_libdvdcss.sh" "$STAGE"
    # Zip names only: libdvdcss.so* / libdvdcss2 packages — not debian patch filenames.
    if unzip -Z1 "$ZIP" | grep -E '(^|/)libdvdcss(\.so|$)|(^|/)libdvdcss2'; then
        echo "ERROR: zip contains libdvdcss binary" >&2
        exit 1
    fi
    sh256 "$ZIP" | awk -v z="$ZIP" '{print $1 "  " z}' > "$SHAFILE"
    echo "ZIP $ZIP"
    cat "$SHAFILE"
fi

echo "Staged $STAGE"
exit 0
