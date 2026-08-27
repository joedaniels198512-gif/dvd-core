#!/bin/bash
# Cross-compile MiSTer_DVD from the recorded Main_MiSTer snapshot + DVD overlay.
# Does not deploy. Refuses VFPv4 (vfma/vfms/vfnma/vfnms).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/.src"
UPSTREAM="$(tr -d ' \n' < "$HERE/UPSTREAM_COMMIT")"
REPO_URL="https://github.com/MiSTer-devel/Main_MiSTer.git"
CROSS="arm-unknown-linux-gnueabihf-"
CC="${CROSS}gcc"
OBJDUMP="${CROSS}objdump"
# Do NOT use armv7-unknown-linux-gnueabihf-gcc (VFPv4 SIGILL on Cortex-A9).

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "ERROR: $CC not found" >&2
    exit 1
fi

if [ ! -d "$SRC" ]; then
    echo "Cloning Main_MiSTer $UPSTREAM"
    git clone "$REPO_URL" "$SRC"
    git -C "$SRC" fetch --depth 1 origin "$UPSTREAM"
    git -C "$SRC" checkout --force "$UPSTREAM"
elif [ -d "$SRC/.git" ]; then
    got="$(git -C "$SRC" rev-parse HEAD)"
    if [ "$got" != "$UPSTREAM" ]; then
        echo "ERROR: $SRC is $got, expected $UPSTREAM" >&2
        exit 1
    fi
fi

# Restore hook sites so apply_dvd_hooks.py stays idempotent on a dirty tree.
if [ -d "$SRC/.git" ]; then
    git -C "$SRC" checkout -- fpga_io.cpp user_io.cpp Makefile
    git -C "$SRC" clean -f -- dvd_main.cpp dvd_main.h
fi

python3 "$HERE/apply_dvd_hooks.py"

echo "Building MiSTer_DVD (upstream $UPSTREAM)"
make -C "$SRC" 2>&1 | tee "$HERE/build.log"

BIN="$SRC/bin/MiSTer_DVD"
[ -f "$BIN" ] || { echo "ERROR: missing $BIN" >&2; exit 1; }

echo "objdump VFPv4 gate..."
if "$OBJDUMP" -d "$BIN" | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' >/dev/null; then
    echo "ERROR: VFPv4 instructions present — refusing $BIN" >&2
    "$OBJDUMP" -d "$BIN" | grep -E '[[:space:]](vfma|vfms|vfnma|vfnms)\.' | head
    exit 1
fi
echo "VFPv4 gate passed"

mkdir -p "$HERE/bin"
cp -f "$BIN" "$HERE/bin/MiSTer_DVD"
ls -l "$HERE/bin/MiSTer_DVD"
echo "OK $HERE/bin/MiSTer_DVD"
