#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-compile MiSTer_DVD_Appliance from the recorded Main_MiSTer snapshot
# plus the Appliance overlay. Does not overwrite main-dvd/bin/MiSTer_DVD.
# Does not deploy. Requires ARM GNU-A 10.2.1 arm-none-linux-gnueabihf.
# Refuses Homebrew GCC 15 (GLIBCXX_3.4.32) and armv7-unknown (VFPv4).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/.src"
TC_ROOT="$HERE/toolchains"
UPSTREAM="$(tr -d ' \n' < "$HERE/UPSTREAM_COMMIT")"
REPO_URL="https://github.com/MiSTer-devel/Main_MiSTer.git"

# Official Main_MiSTer toolchain (GCC 10.2.1). See TOOLCHAIN.md.
CROSS="arm-none-linux-gnueabihf-"
REQUIRED_GCC="10.2.1"
MAX_GLIBCXX_MINOR=28
ARM_GNU_A="10.2-2020.11"
TC_BASE_URL="https://developer.arm.com/-/media/Files/downloads/gnu-a/${ARM_GNU_A}/binrel"

# Do NOT use these:
#   arm-unknown-linux-gnueabihf-gcc  (Homebrew 15.2 → GLIBCXX_3.4.32)
#   armv7-unknown-linux-gnueabihf-gcc (VFPv4 SIGILL on Cortex-A9)

die() { echo "ERROR: $*" >&2; exit 1; }

info() { echo "$*"; }

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

toolchain_triple() {
    # Host of the *compiler binary* (not the target). Docker linux/arm64 on
    # Apple Silicon uses the aarch64-hosted 10.2.1 tarball; amd64 matches
    # Main_MiSTer .devcontainer exactly.
    case "${1:-}" in
        aarch64|arm64) echo "aarch64" ;;
        x86_64|amd64) echo "x86_64" ;;
        *) die "unsupported toolchain host arch: $1" ;;
    esac
}

ensure_toolchain() {
    local host_arch tarball dest
    host_arch=$(toolchain_triple "$1")
    tarball="gcc-arm-${ARM_GNU_A}-${host_arch}-arm-none-linux-gnueabihf.tar.xz"
    dest="$TC_ROOT/gcc-arm-${ARM_GNU_A}-${host_arch}-arm-none-linux-gnueabihf"
    mkdir -p "$TC_ROOT"
    if [ -x "$dest/bin/${CROSS}gcc" ]; then
        echo "$dest"
        return
    fi
    echo "Downloading ARM GNU-A ${ARM_GNU_A} (${host_arch} host)…" >&2
    need_cmd curl
    curl -fL --retry 3 -o "$TC_ROOT/$tarball" "$TC_BASE_URL/$tarball"
    case "$tarball" in
        gcc-arm-10.2-2020.11-aarch64-arm-none-linux-gnueabihf.tar.xz)
            echo "d169f9196e3a6c4248ee79ca85987ebce0e4ea9174c1f8d51af9b28fecf22da1  $TC_ROOT/$tarball" | shasum -a 256 -c -
            ;;
    esac
    echo "Extracting $tarball" >&2
    tar -xJf "$TC_ROOT/$tarball" -C "$TC_ROOT"
    [ -x "$dest/bin/${CROSS}gcc" ] || die "extract did not produce $dest/bin/${CROSS}gcc"
    echo "$dest"
}

gcc_version_ok() {
    local gcc=$1
    local ver
    ver=$("$gcc" -dumpversion 2>/dev/null || true)
    [ "$ver" = "$REQUIRED_GCC" ]
}

check_glibcxx_gate() {
    local bin=$1
    local readelf=$2
    local ver minor
    local worst=0
    while IFS= read -r ver; do
        [ -n "$ver" ] || continue
        minor=${ver#GLIBCXX_3.4.}
        case "$minor" in
            ''|*[!0-9]*) continue ;;
        esac
        if [ "$minor" -gt "$worst" ]; then
            worst=$minor
        fi
        if [ "$minor" -gt "$MAX_GLIBCXX_MINOR" ]; then
            die "GLIBCXX gate: $ver required (SS1 libstdc++ max is GLIBCXX_3.4.${MAX_GLIBCXX_MINOR})"
        fi
    done < <("$readelf" -V "$bin" | sed -n 's/.*Name: \(GLIBCXX_3\.4\.[0-9][0-9]*\).*/\1/p' | sort -u)
    if [ "$worst" -eq 0 ]; then
        die "GLIBCXX gate: no GLIBCXX_3.4.x versions found (readelf failed?)"
    fi
    info "GLIBCXX gate passed (highest required 3.4.$worst, SS1 max 3.4.$MAX_GLIBCXX_MINOR)"
}

docker_make() {
    local platform=$1
    local tc=$2
    local image="mister-dvd-main-build"
    need_cmd docker
    info "Building helper image $image ($platform)"
    docker build --platform "$platform" -t "$image" -f "$HERE/Dockerfile.build" "$HERE"
    info "make with GCC $REQUIRED_GCC in Docker $platform"
    docker run --rm --platform "$platform" \
        -v "$tc:/opt/gcc-arm:ro" \
        -v "$SRC:/src" \
        -w /src \
        "$image" \
        bash -c 'export PATH=/opt/gcc-arm/bin:$PATH
            set -euo pipefail
            gcc=arm-none-linux-gnueabihf-gcc
            echo "compiler: $($gcc --version | head -1)"
            echo "dumpversion: $($gcc -dumpversion)"
            [ "$($gcc -dumpversion)" = "10.2.1" ] || { echo "ERROR: not GCC 10.2.1" >&2; exit 1; }
            make clean
            make -j"$(nproc)"'
}

# --- snapshot ---
if [ ! -d "$SRC" ]; then
    echo "Cloning Main_MiSTer $UPSTREAM"
    git clone "$REPO_URL" "$SRC"
    git -C "$SRC" fetch --depth 1 origin "$UPSTREAM"
    git -C "$SRC" checkout --force "$UPSTREAM"
elif [ -d "$SRC/.git" ]; then
    got="$(git -C "$SRC" rev-parse HEAD)"
    if [ "$got" != "$UPSTREAM" ]; then
        die "$SRC is $got, expected $UPSTREAM"
    fi
fi

if [ -d "$SRC/.git" ]; then
    git -C "$SRC" checkout -- fpga_io.cpp user_io.cpp Makefile
    git -C "$SRC" clean -f -- dvd_main.cpp dvd_main.h
fi

python3 "$HERE/apply_dvd_hooks.py"
# Replace the launcher overlay with the Appliance overlay and binary name.
# The original apply_dvd_hooks.py still copies checkpoint dvd_main.* first.
cp -f "$HERE/appliance/dvd_main.cpp" "$SRC/dvd_main.cpp"
cp -f "$HERE/appliance/dvd_main.h" "$SRC/dvd_main.h"
perl -i -pe 's/^PRJ = MiSTer_DVD$/PRJ = MiSTer_DVD_Appliance/' "$SRC/Makefile"
grep -q '^PRJ = MiSTer_DVD_Appliance$' "$SRC/Makefile" \
    || die "Makefile PRJ was not set to MiSTer_DVD_Appliance"
grep -q 'BASE.*= *arm-none-linux-gnueabihf' "$SRC/Makefile" \
    || die "Makefile BASE is not arm-none-linux-gnueabihf (refusing GCC 15 rewrite)"

# --- toolchain: Docker on macOS (Linux ELF compiler); native if already on PATH ---
NATIVE_GCC="$(command -v ${CROSS}gcc || true)"
if [ -n "$NATIVE_GCC" ] && gcc_version_ok "$NATIVE_GCC"; then
    case "$(uname -s)" in
        Linux)
            info "Using native $NATIVE_GCC"
            export PATH="$(dirname "$NATIVE_GCC"):$PATH"
            echo "Building MiSTer_DVD_Appliance (upstream $UPSTREAM) with $($NATIVE_GCC --version | head -1)"
            make -C "$SRC" 2>&1 | tee "$HERE/build.log"
            ;;
        *)
            die "$NATIVE_GCC is not a Linux-hosted GCC 10.2.1; use Docker via this script"
            ;;
    esac
else
    need_cmd docker
    DOCKER_ARCH=$(docker info --format '{{.Architecture}}' 2>/dev/null || echo arm64)
    case "$DOCKER_ARCH" in
        aarch64|arm64)
            PLATFORM=linux/arm64
            TC_HOST=aarch64
            ;;
        x86_64|amd64)
            PLATFORM=linux/amd64
            TC_HOST=x86_64
            ;;
        *)
            info "docker arch '$DOCKER_ARCH', defaulting to linux/arm64"
            PLATFORM=linux/arm64
            TC_HOST=aarch64
            ;;
    esac
    TC=$(ensure_toolchain "$TC_HOST")
    TC_GCC="$TC/bin/${CROSS}gcc"
    [ -x "$TC_GCC" ] || die "missing $TC_GCC"
    info "Building MiSTer_DVD_Appliance (upstream $UPSTREAM)"
    docker_make "$PLATFORM" "$TC" 2>&1 | tee "$HERE/build.log"
fi

BIN="$SRC/bin/MiSTer_DVD_Appliance"
[ -f "$BIN" ] || die "missing $BIN"

# Tools for gates: host binutils can inspect ARM ELF; the 10.2.1
# objdump/readelf are Linux binaries and will not run on Darwin.
if command -v "${CROSS}objdump" >/dev/null 2>&1 && "${CROSS}objdump" -h "$BIN" >/dev/null 2>&1; then
    OBJDUMP="${CROSS}objdump"
    READELF="${CROSS}readelf"
elif command -v arm-unknown-linux-gnueabihf-objdump >/dev/null 2>&1; then
    OBJDUMP="arm-unknown-linux-gnueabihf-objdump"
    READELF="arm-unknown-linux-gnueabihf-readelf"
    info "NOTE: using host $OBJDUMP only to inspect the binary (not to compile)"
else
    die "no runnable ARM objdump/readelf to inspect $BIN"
fi

echo "objdump VFPv4 gate..."
if ! "$OBJDUMP" -d "$BIN" >/tmp/mister_dvd_appliance_objdump.txt 2>/tmp/mister_dvd_appliance_objdump.err; then
    die "objdump failed: $(cat /tmp/mister_dvd_appliance_objdump.err)"
fi
if grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' /tmp/mister_dvd_appliance_objdump.txt >/dev/null; then
    echo "ERROR: VFPv4 instructions present — refusing $BIN" >&2
    grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' /tmp/mister_dvd_appliance_objdump.txt | head
    exit 1
fi
echo "VFPv4 gate passed"

check_glibcxx_gate "$BIN" "$READELF"

mkdir -p "$HERE/bin"
cp -f "$BIN" "$HERE/bin/MiSTer_DVD_Appliance"
ls -l "$HERE/bin/MiSTer_DVD_Appliance"
echo "OK $HERE/bin/MiSTer_DVD_Appliance"
