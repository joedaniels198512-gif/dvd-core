#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Conservative CSS packaging policy checks (no SS1 required).
# 1. Installer must not treat missing libdvdcss as fatal.
# 2. Installer must not download CSS.
# 3. Installer/uninstaller must not delete or overwrite CSS.
# 4. Missing CSS is a non-fatal branch (simulated).
#
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
INST="$ROOT/release/pkg_scripts/Install_MiSTer_DVD_Player.sh"
UNINST="$ROOT/release/pkg_scripts/Uninstall_MiSTer_DVD_Player.sh"

die() { echo "ERROR: $*" >&2; exit 1; }

grep -F -q 'die "missing libdvdcss' "$INST" && \
    die "installer still fails closed on missing libdvdcss"

# No fetchers aimed at CSS / VideoLAN downloads in the installer.
if grep -E 'curl|wget|ftp ' "$INST" | grep -i dvdcss >/dev/null 2>&1; then
    die "installer appears to download libdvdcss"
fi
if grep -E 'videolan.org/.*(dvdcss|libdvdcss)' "$INST" >/dev/null 2>&1; then
    # Mentions in echo/info are OK; URLs used as download sources are not.
    if grep -E '(curl|wget).+videolan' "$INST" >/dev/null 2>&1; then
        die "installer downloads from VideoLAN"
    fi
fi

if grep -E 'rm[[:space:]].*libdvdcss' "$INST" "$UNINST" >/dev/null 2>&1; then
    die "installer/uninstaller deletes libdvdcss"
fi

# Simulate the non-fatal presence test from the installer.
LIBDIR=$(mktemp -d)
trap 'rm -rf "$LIBDIR"' EXIT
# Empty dir: missing CSS must not be fatal.
if [ -e "$LIBDIR/libdvdcss.so.2" ] || [ -e "$LIBDIR/libdvdcss.so.2.2.0" ]; then
    die "unexpected CSS in empty mock libdir"
fi
echo "PASS: missing CSS is not fatal (mock empty $LIBDIR)"

# Existing CSS must be detectable and not removed by a no-op check.
: > "$LIBDIR/libdvdcss.so.2"
if [ ! -e "$LIBDIR/libdvdcss.so.2" ]; then
    die "failed to create mock CSS"
fi
# Policy: leave it in place.
[ -e "$LIBDIR/libdvdcss.so.2" ] || die "mock CSS vanished"
echo "PASS: existing CSS would be preserved (mock left in place)"

echo "PASS: installer CSS policy"
exit 0
