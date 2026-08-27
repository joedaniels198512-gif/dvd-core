#!/bin/bash
# Assemble MiSTer DVD Player 0.1.0-private-beta ZIP.
# Does not deploy, commit, or run Quartus.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="0.1.0-private-beta"
STAGE="$ROOT/release/MiSTer_DVD_Player_${VER}"
ZIP="$ROOT/release/MiSTer_DVD_Player_${VER}.zip"
SHAFILE="$ROOT/release/MiSTer_DVD_Player_${VER}.sha256"
DIST="$ROOT/player/dist"
DL="$HOME/Downloads"
FF_TARBALL="$ROOT/player/.build/ffmpeg-6.0.1.tar.xz"
FF_SRC="$ROOT/player/.build/ffmpeg-6.0.1"

RBF_SHA_EXPECT="55e6b114bc34fe1d314ad7ec94e1944627968d44c42e947d13caba23470576ae"
PLAYER_SHA_EXPECT="09d176985f5ecacca30860796b75e250e0ac6389c153e98545b34f1d336b18ff"
RIP_SHA_EXPECT="42aec76ee04e6eb5b70c962ab764a6eeed897f2876cfc45cb0a39b39df51cecf"
LAUNCHER_OLD="e9da16cae4b01bdf5e32eb032788ee25ecd194c673cad42505b36e9bcddf0e79"

sh256() { shasum -a 256 "$1" | awk '{print $1}'; }

echo "Staging $STAGE"
rm -rf "$STAGE"
mkdir -p \
    "$STAGE/DVD/dev/rip-lib" \
    "$STAGE/DVD/bin" \
    "$STAGE/DVD/lib" \
    "$STAGE/DVD/data" \
    "$STAGE/DVD/logs" \
    "$STAGE/DVD/cache" \
    "$STAGE/DVD/isos" \
    "$STAGE/DVD/backup" \
    "$STAGE/Scripts" \
    "$STAGE/LICENSES" \
    "$STAGE/SOURCES/mister-dvd-player-arm"

# --- binaries (proven DVD/dev layout) ---
cp -f "$DIST/DVD_V1_OSD_Test.rbf" "$STAGE/MiSTer_DVD_Player.rbf"
cp -f "$DIST/DVD_V1_OSD_Test.rbf" "$STAGE/DVD/MiSTer_DVD_Player.rbf"
cp -f "$DIST/dvd_launcher" "$STAGE/DVD/dev/"
cp -f "$DIST/dvd_av_threaded_test" "$STAGE/DVD/dev/"
cp -f "$DIST/dvd_rip_iso" "$STAGE/DVD/dev/"
cp -f "$DIST/dvdbackup" "$STAGE/DVD/dev/"
cp -f "$DIST/genisoimage" "$STAGE/DVD/dev/"
cp -f "$DIST/magic.mgc" "$STAGE/DVD/dev/"
cp -f "$DIST/magic.mgc" "$STAGE/DVD/data/"
cp -a "$DIST/rip-lib/." "$STAGE/DVD/dev/rip-lib/"
# also in DVD/lib so LD_LIBRARY_PATH=/media/fat/DVD/lib covers genisoimage
cp -a "$DIST/rip-lib/." "$STAGE/DVD/lib/"

cp -f "$DL/dvdnav-lib-extracted/usr/lib/arm-linux-gnueabihf/libdvdnav.so.4.3.0" "$STAGE/DVD/lib/"
cp -f "$STAGE/DVD/lib/libdvdnav.so.4.3.0" "$STAGE/DVD/lib/libdvdnav.so.4"
cp -f "$DL/dvdread-extracted/usr/lib/arm-linux-gnueabihf/libdvdread.so.8.0.0" "$STAGE/DVD/lib/"
cp -f "$STAGE/DVD/lib/libdvdread.so.8.0.0" "$STAGE/DVD/lib/libdvdread.so.8"
cp -f "$DL/dvdcss-extracted/usr/lib/arm-linux-gnueabihf/libdvdcss.so.2.2.0" "$STAGE/DVD/lib/"
cp -f "$STAGE/DVD/lib/libdvdcss.so.2.2.0" "$STAGE/DVD/lib/libdvdcss.so.2"
# FAT SD cards cannot store symlinks — materialise rip-lib sonames as files
python3 - <<'PY'
from pathlib import Path
for root in [
    Path("/Users/jarvisaiassistant/Projects/dvd-core/release/MiSTer_DVD_Player_0.1.0-private-beta/DVD/lib"),
    Path("/Users/jarvisaiassistant/Projects/dvd-core/release/MiSTer_DVD_Player_0.1.0-private-beta/DVD/dev/rip-lib"),
]:
    if not root.is_dir():
        continue
    for p in list(root.iterdir()):
        if p.is_symlink():
            target = p.resolve()
            p.unlink()
            p.write_bytes(target.read_bytes())
print("symlinks flattened")
PY

cp -f "$ROOT/player/tools/dvd_autostart_daemon.sh" "$STAGE/DVD/bin/"
chmod +x "$STAGE/DVD/bin/dvd_autostart_daemon.sh" \
         "$STAGE/DVD/dev/"*

MAIN_BIN="$ROOT/main-dvd/bin/MiSTer_DVD"
if [ ! -f "$MAIN_BIN" ]; then
    MAIN_BIN="$ROOT/main-dvd/.src/bin/MiSTer_DVD"
fi
if [ -f "$MAIN_BIN" ]; then
    cp -f "$MAIN_BIN" "$STAGE/MiSTer_DVD"
    chmod +x "$STAGE/MiSTer_DVD"
    echo "Packed MiSTer_DVD from $MAIN_BIN"
else
    echo "WARN: MiSTer_DVD binary not built yet; ZIP will not include it" >&2
fi

# empty dirs keep
: > "$STAGE/DVD/logs/.keep"
: > "$STAGE/DVD/cache/.keep"
: > "$STAGE/DVD/isos/.keep"
echo "$VER" > "$STAGE/DVD/VERSION"

# --- scripts / docs ---
python3 - <<'PY'
from pathlib import Path
root = Path("/Users/jarvisaiassistant/Projects/dvd-core")
stage = root / "release" / "MiSTer_DVD_Player_0.1.0-private-beta"
for src, dest in [
    (root / "release/pkg_scripts/Install_MiSTer_DVD_Player.sh", stage / "Scripts/Install_MiSTer_DVD_Player.sh"),
    (root / "release/pkg_scripts/Uninstall_MiSTer_DVD_Player.sh", stage / "Scripts/Uninstall_MiSTer_DVD_Player.sh"),
    (root / "release/pkg_scripts/Export_MiSTer_DVD_Player_Logs.sh", stage / "Scripts/Export_MiSTer_DVD_Player_Logs.sh"),
    (root / "release/pkg_docs/MiSTer_DVD_Player_README.txt", stage / "MiSTer_DVD_Player_README.txt"),
    (root / "release/pkg_docs/MiSTer_DVD_Player_TEST_CHECKLIST.txt", stage / "MiSTer_DVD_Player_TEST_CHECKLIST.txt"),
    (root / "release/pkg_docs/MiSTer_DVD_Player_CHANGELOG.txt", stage / "MiSTer_DVD_Player_CHANGELOG.txt"),
    (root / "release/pkg_docs/SOURCE_INFO.txt", stage / "SOURCES/SOURCE_INFO.txt"),
]:
    t = src.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    dest.write_bytes(t)
    if dest.suffix == ".sh":
        dest.chmod(0o755)
print("copied docs/scripts LF")
PY

# --- licences ---
cp -f "$FF_SRC/COPYING.LGPLv2.1" "$STAGE/LICENSES/FFmpeg-COPYING.LGPLv2.1"
cp -f "$FF_SRC/COPYING.GPLv2" "$STAGE/LICENSES/COPYING.GPLv2"
cp -f "$FF_SRC/COPYING.GPLv3" "$STAGE/LICENSES/COPYING.GPLv3"
cp -f "$FF_SRC/LICENSE.md" "$STAGE/LICENSES/FFmpeg-LICENSE.md"
cp -f "$DL/dvdbackup-extracted/usr/share/doc/dvdbackup/copyright" "$STAGE/LICENSES/dvdbackup.copyright"
cp -f "$DL/dvdread-extracted/usr/share/doc/libdvdread8/copyright" "$STAGE/LICENSES/libdvdread.copyright"
cp -f "$DL/dvdnav-lib-extracted/usr/share/doc/libdvdnav4/copyright" "$STAGE/LICENSES/libdvdnav.copyright"
cp -f "$DL/dvdcss-extracted/usr/share/doc/libdvdcss2/copyright" "$STAGE/LICENSES/libdvdcss.copyright"
cp -f "$DL/genisoimage-extracted/COPYRIGHT.genisoimage" "$STAGE/LICENSES/genisoimage.copyright"
cp -f "$ROOT/release/pkg_docs/zlib.LICENSE.txt" "$STAGE/LICENSES/"
cp -f "$ROOT/release/pkg_docs/bzip2.LICENSE.txt" "$STAGE/LICENSES/"

# --- our ARM source snapshot ---
ARM="$STAGE/SOURCES/mister-dvd-player-arm"
for f in \
    dvd_launcher.c dvd_library.c dvd_library.h dvd_rip_iso.c \
    dvd_av_threaded_test.c dvd_autostart_daemon.sh \
    build_dvd_launcher.sh build_dvd_rip_iso.sh build_dvd_av_threaded_test.sh \
    install_dvd_autostart.sh
do
    cp -f "$ROOT/player/tools/$f" "$ARM/"
done
cp -f "$ROOT/player/build_mac.sh" "$ARM/"
mkdir -p "$ARM/main-dvd"
cp -f "$ROOT/main-dvd/dvd_main.cpp" "$ROOT/main-dvd/dvd_main.h" \
      "$ROOT/main-dvd/apply_dvd_hooks.py" "$ROOT/main-dvd/build_mister_dvd.sh" \
      "$ROOT/main-dvd/UPSTREAM_COMMIT" "$ROOT/main-dvd/UPSTREAM.txt" \
      "$ARM/main-dvd/"

# --- upstream sources (best-effort download) ---
mkdir -p "$STAGE/SOURCES" "$ROOT/release/src-cache"
cp -f "$FF_TARBALL" "$STAGE/SOURCES/ffmpeg-6.0.1.tar.xz"
cp -f "$ROOT/release/src-cache/"* "$STAGE/SOURCES/" 2>/dev/null || true

fetch() {
    local url=$1 dest=$2
    if [ -f "$dest" ]; then
        echo "have $dest"
        return 0
    fi
    echo "fetch $url"
    curl -L --fail --retry 2 --max-time 120 -o "$dest" "$url" && return 0
    echo "WARN: failed $url" >&2
    rm -f "$dest"
    return 1
}

SRC="$STAGE/SOURCES"
CACHE="$ROOT/release/src-cache"
getsrc() {
    local url=$1 name=$2
    if [ -f "$CACHE/$name" ]; then
        cp -f "$CACHE/$name" "$SRC/$name"
        echo "have $name"
        return 0
    fi
    if fetch "$url" "$CACHE/$name"; then
        cp -f "$CACHE/$name" "$SRC/$name"
        return 0
    fi
    return 1
}

getsrc "https://download.videolan.org/pub/videolan/libdvdread/6.1.1/libdvdread-6.1.1.tar.bz2" "libdvdread-6.1.1.tar.bz2" || true
getsrc "https://download.videolan.org/pub/videolan/libdvdnav/6.1.0/libdvdnav-6.1.0.tar.bz2" "libdvdnav-6.1.0.tar.bz2" || true
getsrc "https://download.videolan.org/pub/libdvdcss/1.4.2/libdvdcss-1.4.2.tar.bz2" "libdvdcss-1.4.2.tar.bz2" || true
getsrc "https://sourceforge.net/projects/dvdbackup/files/dvdbackup/dvdbackup-0.4.2/dvdbackup-0.4.2.tar.gz/download" "dvdbackup-0.4.2.tar.gz" || \
  getsrc "http://deb.debian.org/debian/pool/main/d/dvdbackup/dvdbackup_0.4.2.orig.tar.xz" "dvdbackup_0.4.2.orig.tar.xz" || true
getsrc "http://deb.debian.org/debian/pool/main/c/cdrkit/cdrkit_1.1.11.orig.tar.gz" "cdrkit_1.1.11.orig.tar.gz" || true
getsrc "https://github.com/file/file/archive/refs/tags/FILE5_39.tar.gz" "file-FILE5_39.tar.gz" || true
getsrc "https://zlib.net/fossils/zlib-1.2.11.tar.gz" "zlib-1.2.11.tar.gz" || true
getsrc "https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz" "bzip2-1.0.8.tar.gz" || true

# --- verify critical hashes ---
rbf=$(sh256 "$STAGE/MiSTer_DVD_Player.rbf")
player=$(sh256 "$STAGE/DVD/dev/dvd_av_threaded_test")
rip=$(sh256 "$STAGE/DVD/dev/dvd_rip_iso")
launcher=$(sh256 "$STAGE/DVD/dev/dvd_launcher")
echo "RBF      $rbf"
echo "PLAYER   $player"
echo "RIP      $rip"
echo "LAUNCHER $launcher"
[ "$rbf" = "$RBF_SHA_EXPECT" ] || { echo "ERROR: RBF hash mismatch"; exit 1; }
[ "$player" = "$PLAYER_SHA_EXPECT" ] || { echo "ERROR: player hash mismatch"; exit 1; }
[ "$rip" = "$RIP_SHA_EXPECT" ] || { echo "ERROR: rip helper hash mismatch"; exit 1; }
if [ "$launcher" = "$LAUNCHER_OLD" ]; then
    echo "NOTE: launcher hash still pre-version-footer"
else
    echo "NOTE: launcher hash changed (version footer 0.1.0); old was $LAUNCHER_OLD"
fi

# --- no leaked paths / IPs in scripts ---
if grep -R "/Users/jarvis" "$STAGE/Scripts" "$STAGE/DVD/bin" >/dev/null 2>&1; then
    echo "ERROR: host path leaked into package scripts"
    exit 1
fi
if grep -R "192.168.1.212" "$STAGE" --include='*.sh' --include='*.txt' >/dev/null 2>&1; then
    echo "ERROR: SSH IP leaked"
    exit 1
fi

# --- checksums of every file ---
(
    cd "$STAGE"
    find . -type f ! -name 'MiSTer_DVD_Player_CHECKSUMS.txt' | LC_ALL=C sort | while read -r f; do
        shasum -a 256 "$f"
    done
) > "$STAGE/MiSTer_DVD_Player_CHECKSUMS.txt"

# --- zip contents at archive root ---
rm -f "$ZIP"
(
    cd "$STAGE"
    # zip from inside staging so ZIP root is RBF/DVD/Scripts/...
    zip -r -X "$ZIP" . \
        -x "*.DS_Store" \
        -x "*__MACOSX*"
)

sh256 "$ZIP" | awk -v z="$ZIP" '{print $1 "  " z}' > "$SHAFILE"

echo
echo "ZIP $ZIP"
ls -lh "$ZIP"
cat "$SHAFILE"
echo
echo "tree:"
(cd "$STAGE" && find . -print | LC_ALL=C sort)
