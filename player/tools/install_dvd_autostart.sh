#!/bin/sh
#
# Idempotent installer for MiSTer DVD Player auto-launch.
#
# Intended to run on the SS1 as:
#   /media/fat/Scripts/Install_MiSTer_DVD_Player.sh
#
# Does not overwrite user-startup.sh. Appends one marked block if missing.
# Does not write names.txt: MiSTer_DVD_Player.rbf already displays as
# "MiSTer DVD Player" (underscores become spaces).
#
# Usage:
#   install_dvd_autostart.sh [--rbf PATH] [--skip-rbf] [--no-start]
#

set -e

DVD_ROOT=/media/fat/DVD
LAUNCHER=$DVD_ROOT/dev/dvd_launcher
PLAYER=$DVD_ROOT/dev/dvd_av_threaded_test
LIBDIR=$DVD_ROOT/lib
LOGDIR=$DVD_ROOT/logs
CACHEDIR=$DVD_ROOT/cache
BINDIR=$DVD_ROOT/bin
DAEMON_DST=$BINDIR/dvd_autostart_daemon.sh
RBF_DST=/media/fat/MiSTer_DVD_Player.rbf
STARTUP=/media/fat/linux/user-startup.sh
STARTUP_TEMPLATE=/media/fat/linux/_user-startup.sh
SCRIPTS_DIR=/media/fat/Scripts
SCRIPTS_COPY=$SCRIPTS_DIR/Install_MiSTer_DVD_Player.sh

MARKER_BEGIN="# MiSTer DVD Player auto-launch"
MARKER_END="# END MiSTer DVD Player auto-launch"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DAEMON_SRC=$HERE/dvd_autostart_daemon.sh
if [ ! -f "$DAEMON_SRC" ] && [ -f "$DAEMON_DST" ]; then
    DAEMON_SRC=$DAEMON_DST
fi

RBF_SRC=""
SKIP_RBF=0
START_NOW=1

usage() {
    echo "Usage: $0 [--rbf PATH] [--skip-rbf] [--no-start]"
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --rbf)
            [ $# -ge 2 ] || usage
            RBF_SRC=$2
            shift 2
            ;;
        --skip-rbf)
            SKIP_RBF=1
            shift
            ;;
        --no-start)
            START_NOW=0
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

die() {
    echo "ERROR: $*" >&2
    exit 1
}

info() {
    echo "$*"
}

find_rbf() {
    if [ -n "$RBF_SRC" ]; then
        [ -f "$RBF_SRC" ] || die "RBF not found: $RBF_SRC"
        echo "$RBF_SRC"
        return 0
    fi
    for p in \
        /media/fat/DVD_CRT_Field_Test.rbf \
        /media/fat/DVD.rbf \
        /media/fat/_Console/DVD_CRT_Field_Test.rbf \
        /media/fat/_Console/DVD.rbf \
        "$DVD_ROOT/DVD.rbf" \
        "$DVD_ROOT/DVD_CRT_Field_Test.rbf"
    do
        if [ -f "$p" ]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

same_file() {
    _a=$1
    _b=$2
    [ -f "$_a" ] && [ -f "$_b" ] || return 1
    if cmp -s "$_a" "$_b" 2>/dev/null; then
        return 0
    fi
    return 1
}

info "MiSTer DVD Player installer"
info "==========================="

[ -f "$LAUNCHER" ] || die "missing launcher: $LAUNCHER"
[ -x "$LAUNCHER" ] || chmod +x "$LAUNCHER"
[ -f "$PLAYER" ] || die "missing player: $PLAYER"
[ -x "$PLAYER" ] || chmod +x "$PLAYER"
[ -d "$LIBDIR" ] || die "missing library dir: $LIBDIR"
ls "$LIBDIR"/libdvdread.so* >/dev/null 2>&1 || \
    die "missing libdvdread in $LIBDIR"
[ -f "$DAEMON_SRC" ] || die "missing daemon source: $DAEMON_SRC"

mkdir -p "$BINDIR" "$LOGDIR" "$CACHEDIR" "$DVD_ROOT/isos" "$SCRIPTS_DIR" \
         /media/fat/linux

if same_file "$DAEMON_SRC" "$DAEMON_DST"; then
    info "Daemon already installed: $DAEMON_DST"
else
    cp -f "$DAEMON_SRC" "$DAEMON_DST"
    info "Installed daemon: $DAEMON_DST"
fi
chmod +x "$DAEMON_DST" "$LAUNCHER" "$PLAYER"

if [ "$SKIP_RBF" -eq 0 ]; then
    FOUND=$(find_rbf) || die "no source RBF found. Pass --rbf PATH or --skip-rbf"
    info "RBF source: $FOUND"
    info "RBF dest:   $RBF_DST"
    if same_file "$FOUND" "$RBF_DST"; then
        info "RBF already installed (identical)"
    else
        cp -f "$FOUND" "$RBF_DST"
        info "Copied RBF to $RBF_DST"
    fi
    info "names.txt: not modified (filename already displays as MiSTer DVD Player)"
else
    info "Skipping RBF copy (--skip-rbf)"
fi

if [ ! -f "$STARTUP" ]; then
    if [ -f "$STARTUP_TEMPLATE" ]; then
        cp "$STARTUP_TEMPLATE" "$STARTUP"
        info "Created $STARTUP from _user-startup.sh template"
    else
        printf '%s\n' '#!/bin/sh' 'echo "***" $1 "***"' > "$STARTUP"
        info "Created new $STARTUP"
    fi
fi
chmod +x "$STARTUP" 2>/dev/null || true

if grep -F -q "$MARKER_BEGIN" "$STARTUP"; then
    info "user-startup.sh already contains MiSTer DVD Player auto-launch block"
else
    {
        echo ""
        echo "$MARKER_BEGIN"
        echo 'if [ "$1" != "stop" ]; then'
        echo "  $DAEMON_DST >/dev/null 2>&1 &"
        echo "fi"
        echo "$MARKER_END"
    } >> "$STARTUP"
    info "Appended auto-launch block to $STARTUP"
fi

INSTALLER_SELF=$0
case "$INSTALLER_SELF" in
    /*) ;;
    *) INSTALLER_SELF=$HERE/$(basename "$INSTALLER_SELF") ;;
esac
if [ -f "$INSTALLER_SELF" ] && ! same_file "$INSTALLER_SELF" "$SCRIPTS_COPY"; then
    cp -f "$INSTALLER_SELF" "$SCRIPTS_COPY"
fi
chmod +x "$SCRIPTS_COPY"
info "Scripts copy: $SCRIPTS_COPY"

if [ "$START_NOW" -eq 1 ]; then
    "$DAEMON_DST" >/dev/null 2>&1 &
    info "Daemon start requested (singleton; safe if already running)"
else
    info "Daemon not started (--no-start); will start on next boot"
fi

info ""
info "Done."
info "Main menu item: MiSTer DVD Player  ($RBF_DST)"
info "Internal core id remains DVD (/tmp/CORENAME)."
info "Re-running this installer is safe and will not duplicate entries."
