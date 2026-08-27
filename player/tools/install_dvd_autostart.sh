#!/bin/sh
#
# Idempotent installer for MiSTer DVD Player (custom Main path).
#
# Intended to run on the SS1 as:
#   /media/fat/Scripts/Install_MiSTer_DVD_Player.sh
#
# Installs /media/fat/MiSTer_DVD beside stock Main, adds marked
# [DVD-Player] main=MiSTer_DVD, and removes the old user-startup daemon block.
# Does not start dvd_autostart_daemon. Does not replace /media/fat/MiSTer.
#
# Usage:
#   install_dvd_autostart.sh [--rbf PATH] [--skip-rbf] [--main PATH]
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
MAIN_DST=/media/fat/MiSTer_DVD
INI=/media/fat/MiSTer.ini
STARTUP=/media/fat/linux/user-startup.sh
STARTUP_TEMPLATE=/media/fat/linux/_user-startup.sh
SCRIPTS_DIR=/media/fat/Scripts
SCRIPTS_COPY=$SCRIPTS_DIR/Install_MiSTer_DVD_Player.sh

MARKER_BEGIN="# MiSTer DVD Player auto-launch"
MARKER_END="# END MiSTer DVD Player auto-launch"
INI_BEGIN="# BEGIN MiSTer DVD Player main"
INI_END="# END MiSTer DVD Player main"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DAEMON_SRC=$HERE/dvd_autostart_daemon.sh
if [ ! -f "$DAEMON_SRC" ] && [ -f "$DAEMON_DST" ]; then
    DAEMON_SRC=$DAEMON_DST
fi

RBF_SRC=""
MAIN_SRC=""
SKIP_RBF=0

usage() {
    echo "Usage: $0 [--rbf PATH] [--skip-rbf] [--main PATH]"
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
        --main)
            [ $# -ge 2 ] || usage
            MAIN_SRC=$2
            shift 2
            ;;
        --no-start)
            # Legacy flag: daemon is no longer started.
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

same_file() {
    _a=$1
    _b=$2
    [ -f "$_a" ] && [ -f "$_b" ] || return 1
    if cmp -s "$_a" "$_b" 2>/dev/null; then
        return 0
    fi
    return 1
}

is_numeric_pid() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        0) return 1 ;;
        *) return 0 ;;
    esac
}

cmdline_of() {
    tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null
}

stop_matching() {
    _needle=$1
    for _d in /proc/[0-9]*; do
        _pid=${_d#/proc/}
        is_numeric_pid "$_pid" || continue
        _cmd=$(cmdline_of "$_pid")
        case "$_cmd" in
            *"$_needle"*)
                kill -TERM "$_pid" 2>/dev/null || true
                ;;
        esac
    done
}

strip_marked_block() {
    _file=$1
    _begin=$2
    _end=$3
    [ -f "$_file" ] || return 0
    grep -F -q "$_begin" "$_file" || return 0
    _tmp=$_file.tmp.$$
    awk -v b="$_begin" -v e="$_end" '
        $0 == b { skip=1; next }
        $0 == e { skip=0; next }
        skip { next }
        { print }
    ' "$_file" > "$_tmp"
    mv -f "$_tmp" "$_file"
}

dvd_ini_main() {
    [ -f "$1" ] || return 0
    awk '
        /^[ \t]*[;#]/ { next }
        /^[ \t]*\[/ {
            sec=$0
            sub(/^[ \t]*\[/, "", sec)
            sub(/\].*$/, "", sec)
            gsub(/[ \t]/, "", sec)
            in_sec = 0
            if (tolower(sec) == "dvd-player") in_sec = 1
            next
        }
        in_sec {
            line=$0
            sub(/^[ \t]+/, "", line)
            if (tolower(line) ~ /^main[ \t]*=/) {
                sub(/^[Mm][Aa][Ii][Nn][ \t]*=[ \t]*/, "", line)
                sub(/[ \t\r]+$/, "", line)
                print line
                exit
            }
        }
    ' "$1"
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
        "$DVD_ROOT/DVD_CRT_Field_Test.rbf" \
        /media/fat/MiSTer_DVD_Player.rbf
    do
        if [ -f "$p" ]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

find_main() {
    if [ -n "$MAIN_SRC" ]; then
        [ -f "$MAIN_SRC" ] || die "MiSTer_DVD not found: $MAIN_SRC"
        echo "$MAIN_SRC"
        return 0
    fi
    for p in \
        /media/fat/MiSTer_DVD \
        "$DVD_ROOT/dev/MiSTer_DVD" \
        "$HERE/../../main-dvd/bin/MiSTer_DVD"
    do
        if [ -f "$p" ]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

info "MiSTer DVD Player installer (custom Main)"
info "========================================="

[ -f "$LAUNCHER" ] || die "missing launcher: $LAUNCHER"
[ -x "$LAUNCHER" ] || chmod +x "$LAUNCHER"
[ -f "$PLAYER" ] || die "missing player: $PLAYER"
[ -x "$PLAYER" ] || chmod +x "$PLAYER"
[ -d "$LIBDIR" ] || die "missing library dir: $LIBDIR"
ls "$LIBDIR"/libdvdread.so* >/dev/null 2>&1 || \
    die "missing libdvdread in $LIBDIR"

MAIN_FOUND=$(find_main) || die "missing MiSTer_DVD. Copy it to /media/fat/MiSTer_DVD or pass --main PATH"

mkdir -p "$BINDIR" "$LOGDIR" "$CACHEDIR" "$DVD_ROOT/isos" "$SCRIPTS_DIR" \
         /media/fat/linux /media/fat/games/DVD-Player

if [ -f "$DAEMON_SRC" ]; then
    if same_file "$DAEMON_SRC" "$DAEMON_DST"; then
        info "Daemon source already at $DAEMON_DST (not started)"
    else
        cp -f "$DAEMON_SRC" "$DAEMON_DST"
        info "Preserved daemon source: $DAEMON_DST (not started)"
    fi
    chmod +x "$DAEMON_DST"
fi
chmod +x "$LAUNCHER" "$PLAYER"

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
else
    info "Skipping RBF copy (--skip-rbf)"
fi

if [ "$MAIN_FOUND" = "$MAIN_DST" ]; then
    info "MiSTer_DVD already at $MAIN_DST"
else
    cp -f "$MAIN_FOUND" "$MAIN_DST"
    info "Installed $MAIN_DST (stock /media/fat/MiSTer was not replaced)"
fi
chmod +x "$MAIN_DST" 2>/dev/null || true

EXISTING_MAIN=""
if [ -f "$INI" ]; then
    EXISTING_MAIN=$(dvd_ini_main "$INI" || true)
fi
if [ -n "$EXISTING_MAIN" ] && [ "$EXISTING_MAIN" != "MiSTer_DVD" ]; then
    die "conflicting $INI [DVD-Player] main=$EXISTING_MAIN (expected MiSTer_DVD). Not overwriting."
fi

if [ ! -f "$INI" ]; then
    printf '%s\n' "$INI_BEGIN" "[DVD-Player]" "main=MiSTer_DVD" "$INI_END" > "$INI"
    info "Created $INI with [DVD-Player] main=MiSTer_DVD"
elif grep -F -q "$INI_BEGIN" "$INI"; then
    strip_marked_block "$INI" "$INI_BEGIN" "$INI_END"
    {
        echo ""
        echo "$INI_BEGIN"
        echo "[DVD-Player]"
        echo "main=MiSTer_DVD"
        echo "$INI_END"
    } >> "$INI"
    info "Updated marked [DVD-Player] main=MiSTer_DVD block in $INI"
elif [ "$EXISTING_MAIN" = "MiSTer_DVD" ]; then
    info "MiSTer.ini already has [DVD-Player] main=MiSTer_DVD (unmarked; left as-is)"
else
    {
        echo ""
        echo "$INI_BEGIN"
        echo "[DVD-Player]"
        echo "main=MiSTer_DVD"
        echo "$INI_END"
    } >> "$INI"
    info "Appended [DVD-Player] main=MiSTer_DVD to $INI"
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

if [ -f "$STARTUP" ] && grep -F -q "$MARKER_BEGIN" "$STARTUP"; then
    strip_marked_block "$STARTUP" "$MARKER_BEGIN" "$MARKER_END"
    info "Removed marked DVD daemon block from $STARTUP"
else
    info "No marked DVD daemon block in user-startup.sh"
fi

stop_matching "dvd_autostart_daemon"
info "Stopped leftover dvd_autostart_daemon if it was running"

INSTALLER_SELF=$0
case "$INSTALLER_SELF" in
    /*) ;;
    *) INSTALLER_SELF=$HERE/$(basename "$INSTALLER_SELF") ;;
esac
if [ -f "$INSTALLER_SELF" ] && ! same_file "$INSTALLER_SELF" "$SCRIPTS_COPY"; then
    cp -f "$INSTALLER_SELF" "$SCRIPTS_COPY"
fi
chmod +x "$SCRIPTS_COPY" 2>/dev/null || true
info "Scripts copy: $SCRIPTS_COPY"

info ""
info "Done."
info "Stock Main: /media/fat/MiSTer"
info "DVD Main:   /media/fat/MiSTer_DVD"
info "INI:        [DVD-Player] main=MiSTer_DVD"
info "Games dir:  /media/fat/games/DVD-Player"
info "Main menu item: MiSTer DVD Player  ($RBF_DST)"
info "Internal core id is DVD-Player (/tmp/CORENAME)."
info "Re-running this installer is safe and will not duplicate entries."
