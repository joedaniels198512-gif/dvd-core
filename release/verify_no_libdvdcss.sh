#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fail if a public package tree contains a libdvdcss binary or archive.
# Source documentation may mention libdvdcss; binaries must not ship.
#
set -eu

if [ "${1:-}" = "" ]; then
    echo "usage: $0 <package-root-directory>" >&2
    exit 2
fi

ROOT=$1
if [ ! -d "$ROOT" ]; then
    echo "ERROR: not a directory: $ROOT" >&2
    exit 1
fi

# Match shipped sonames and common archive names. Case-insensitive.
# Do not match SOURCE_INFO / comments (those are not files named libdvdcss).
hits=$(find "$ROOT" \( -type f -o -type l \) | awk '
    BEGIN { IGNORECASE=1 }
    {
        n=$0
        sub(/^.*\//, "", n)
        if (n ~ /^libdvdcss(\.so.*)?$/) print $0
        if (n ~ /^libdvdcss_.*\.(deb|so|tar|gz|bz2|xz|zip)$/) print $0
        if (n ~ /^libdvdcss2/) print $0
    }
')

if [ -n "$hits" ]; then
    echo "ERROR: public package contains libdvdcss binary/archive:" >&2
    echo "$hits" >&2
    exit 1
fi

echo "PASS: no libdvdcss binary or archive under $ROOT"
exit 0
