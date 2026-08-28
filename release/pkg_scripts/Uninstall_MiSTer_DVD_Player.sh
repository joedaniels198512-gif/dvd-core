#!/bin/sh
#
# MiSTer DVD Player 0.1.0-private-beta uninstaller.
#
# Conservative: stops the player/launcher/legacy daemon, removes only the
# marked autostart block, the marked [DVD-Player] main= block, MiSTer_DVD_Player.rbf,
# and /media/fat/MiSTer_DVD.
#
# Does NOT delete /media/fat/MiSTer (stock Main).
# Does NOT delete /media/fat/DVD/ (library, cache, logs, ISOs, daemon source,
# or a user-supplied libdvdcss.so.2).

# Does NOT touch unrelated cores or configuration.
#

set -e

DVD_ROOT=/media/fat/DVD
DAEMON=$DVD_ROOT/bin/dvd_autostart_daemon.sh
RBF_DST=/media/fat/MiSTer_DVD_Player.rbf
MAIN_DST=/media/fat/MiSTer_DVD
INI=/media/fat/MiSTer.ini
STARTUP=/media/fat/linux/user-startup.sh
MARKER_BEGIN="# MiSTer DVD Player auto-launch"
MARKER_END="# END MiSTer DVD Player auto-launch"
INI_BEGIN="# BEGIN MiSTer DVD Player main"
INI_END="# END MiSTer DVD Player main"

info() {
    echo "$*"
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
    [ -f "$_file" ] || return 1
    grep -F -q "$_begin" "$_file" || return 1
    _tmp=$_file.tmp.$$
    awk -v b="$_begin" -v e="$_end" '
        $0 == b { skip=1; next }
        $0 == e { skip=0; next }
        skip { next }
        { print }
    ' "$_file" > "$_tmp"
    mv -f "$_tmp" "$_file"
    return 0
}

info "MiSTer DVD Player uninstaller"
info "============================="

stop_matching "dvd_autostart_daemon"
stop_matching "dvd_launcher"
stop_matching "dvd_av_threaded_test"
stop_matching "dvd_rip_iso"
stop_matching "dvdbackup"
stop_matching "genisoimage"
sleep 1
stop_matching "dvd_autostart_daemon"
stop_matching "dvd_launcher"
stop_matching "dvd_av_threaded_test"

rm -f /tmp/dvd_autostart.pid /tmp/dvd_launcher.pid /tmp/dvd_rip_status

if strip_marked_block "$STARTUP" "$MARKER_BEGIN" "$MARKER_END"; then
    info "Removed MiSTer DVD Player block from $STARTUP"
else
    info "No MiSTer DVD Player block in user-startup.sh"
fi

if strip_marked_block "$INI" "$INI_BEGIN" "$INI_END"; then
    info "Removed marked [DVD-Player] main= block from $INI"
else
    info "No marked [DVD-Player] main= block in MiSTer.ini (a hand-written main= was left as-is)"
fi

if [ -f "$RBF_DST" ]; then
    rm -f "$RBF_DST"
    info "Removed $RBF_DST"
else
    info "RBF already absent: $RBF_DST"
fi

if [ -f "$MAIN_DST" ]; then
    rm -f "$MAIN_DST"
    info "Removed $MAIN_DST"
else
    info "MiSTer_DVD already absent: $MAIN_DST"
fi

if [ -f /media/fat/MiSTer ]; then
    info "Stock Main left in place: /media/fat/MiSTer"
fi

info ""
info "Uninstall complete."
info "Left in place (library / cache / logs / binaries / daemon source):"
info "  $DVD_ROOT"
if [ -f "$DAEMON" ]; then
    info "  $DAEMON"
fi
info ""
info "To remove those files as well (ISOs, cache, logs):"
info "  delete the DVD folder on the SD card"
info "This uninstaller will not do that automatically."
