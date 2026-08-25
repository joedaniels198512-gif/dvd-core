#!/bin/sh
#
# MiSTer DVD Player auto-launch daemon.
#
# Starts once at boot from /media/fat/linux/user-startup.sh.
# Watches /tmp/CORENAME (MiSTer writes the FPGA CONF_STR name there).
# When the DVD core is loaded, starts dvd_launcher.
# When any other core is loaded, SIGTERM dvd_launcher (which SIGTERMs the
# player child if it is running).
#
# Internal FPGA name is "DVD" (CONF_STR "DVD;;"). Renaming the RBF to
# MiSTer_DVD_Player.rbf does NOT change /tmp/CORENAME.
#
# BusyBox-safe: no pgrep/pkill. PID files + kill -0 + /proc/PID/cmdline.
#

DVD_ROOT=/media/fat/DVD
LAUNCHER=$DVD_ROOT/dev/dvd_launcher
LIBDIR=$DVD_ROOT/lib
LOGDIR=$DVD_ROOT/logs

DAEMON_PID=/tmp/dvd_autostart.pid
LAUNCHER_PID=/tmp/dvd_launcher.pid

LAUNCHER_LOG=$LOGDIR/launcher.log
LAUNCHER_PREV=$LOGDIR/launcher.previous.log
DAEMON_LOG=$LOGDIR/autostart.log

POLL_SEC=0.5
TERM_WAIT_SEC=8
KILL_WAIT_SEC=2
RESPAWN_WINDOW_SEC=5
RESPAWN_MAX=3

log() {
    echo "DVD AUTOSTART: $*"
    echo "DVD AUTOSTART: $*" >> "$DAEMON_LOG" 2>/dev/null
}

trim() {
    tr -d '\r\n \t' < "$1" 2>/dev/null
}

is_numeric_pid() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        0) return 1 ;;
        *) return 0 ;;
    esac
}

read_pidfile() {
    _pid=""
    if [ -f "$1" ]; then
        read _pid _rest < "$1" 2>/dev/null || _pid=""
    fi
    echo "$_pid"
}

cmdline_of() {
    tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null
}

pid_matches() {
    _pid=$1
    _needle=$2
    is_numeric_pid "$_pid" || return 1
    [ -d "/proc/$_pid" ] || return 1
    kill -0 "$_pid" 2>/dev/null || return 1
    _cmd=$(cmdline_of "$_pid")
    case "$_cmd" in
        *"$_needle"*) return 0 ;;
    esac
    return 1
}

clear_stale_pidfile() {
    _file=$1
    _needle=$2
    _pid=$(read_pidfile "$_file")
    if [ -z "$_pid" ]; then
        rm -f "$_file"
        return
    fi
    if pid_matches "$_pid" "$_needle"; then
        return
    fi
    rm -f "$_file"
}

name_is_dvd() {
    _n=$(echo "$1" | tr 'A-Z' 'a-z')
    _n=${_n%.rbf}
    _n=${_n##*/}
    case "$_n" in
        dvd|dvd_*|mister_dvd_player*) return 0 ;;
    esac
    return 1
}

dvd_core_active() {
    _core=""
    _rbf=""
    [ -f /tmp/CORENAME ] && _core=$(trim /tmp/CORENAME)
    [ -f /tmp/RBFNAME ] && _rbf=$(trim /tmp/RBFNAME)
    if name_is_dvd "$_core"; then
        return 0
    fi
    if name_is_dvd "$_rbf"; then
        return 0
    fi
    return 1
}

launcher_running() {
    _pid=$(read_pidfile "$LAUNCHER_PID")
    if pid_matches "$_pid" "dvd_launcher"; then
        case "$(cmdline_of "$_pid")" in
            *dvd_autostart_daemon*) return 1 ;;
        esac
        return 0
    fi
    return 1
}

rotate_launcher_log() {
    if [ -f "$LAUNCHER_LOG" ]; then
        mv -f "$LAUNCHER_LOG" "$LAUNCHER_PREV" 2>/dev/null || true
    fi
}

start_launcher() {
    if launcher_running; then
        return 0
    fi
    if [ ! -x "$LAUNCHER" ]; then
        log "launcher missing: $LAUNCHER"
        return 1
    fi
    mkdir -p "$LOGDIR"
    rotate_launcher_log
    clear_stale_pidfile "$LAUNCHER_PID" "dvd_launcher"

    LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
    "$LAUNCHER" </dev/null >>"$LAUNCHER_LOG" 2>&1 &
    _lpid=$!
    echo "$_lpid" > "$LAUNCHER_PID"
    sleep 0.1 2>/dev/null || true
    if pid_matches "$_lpid" "dvd_launcher"; then
        log "launcher started pid=$_lpid"
        return 0
    fi
    log "launcher failed to start"
    rm -f "$LAUNCHER_PID"
    return 1
}

wait_pid_gone() {
    _pid=$1
    _needle=$2
    _secs=$3
    _i=0
    while [ "$_i" -lt "$_secs" ]; do
        pid_matches "$_pid" "$_needle" || return 0
        sleep 1
        _i=$((_i + 1))
    done
    pid_matches "$_pid" "$_needle" && return 1
    return 0
}

stop_verified() {
    _pid=$1
    _needle=$2
    pid_matches "$_pid" "$_needle" || return 0
    kill -TERM "$_pid" 2>/dev/null || true
    if wait_pid_gone "$_pid" "$_needle" "$TERM_WAIT_SEC"; then
        return 0
    fi
    kill -KILL "$_pid" 2>/dev/null || true
    wait_pid_gone "$_pid" "$_needle" "$KILL_WAIT_SEC" || true
}

stop_orphans() {
    for _d in /proc/[0-9]*; do
        _pid=${_d#/proc/}
        is_numeric_pid "$_pid" || continue
        _cmd=$(cmdline_of "$_pid")
        case "$_cmd" in
            *dvd_autostart_daemon*) continue ;;
            *dvd_launcher*|*dvd_av_threaded_test*)
                kill -TERM "$_pid" 2>/dev/null || true
                ;;
        esac
    done
    sleep 1
    for _d in /proc/[0-9]*; do
        _pid=${_d#/proc/}
        is_numeric_pid "$_pid" || continue
        _cmd=$(cmdline_of "$_pid")
        case "$_cmd" in
            *dvd_autostart_daemon*) continue ;;
            *dvd_launcher*|*dvd_av_threaded_test*)
                kill -KILL "$_pid" 2>/dev/null || true
                ;;
        esac
    done
}

stop_launcher() {
    _pid=$(read_pidfile "$LAUNCHER_PID")
    if pid_matches "$_pid" "dvd_launcher"; then
        stop_verified "$_pid" "dvd_launcher"
    fi
    rm -f "$LAUNCHER_PID"
    stop_orphans
    log "launcher stopped"
}

claim_singleton() {
    clear_stale_pidfile "$DAEMON_PID" "dvd_autostart_daemon"
    _old=$(read_pidfile "$DAEMON_PID")
    if pid_matches "$_old" "dvd_autostart_daemon"; then
        echo "DVD AUTOSTART: already running pid=$_old"
        exit 0
    fi
    echo $$ > "$DAEMON_PID"
}

on_exit() {
    rm -f "$DAEMON_PID"
}

on_signal() {
    if [ "$in_dvd" = "1" ]; then
        stop_launcher
    fi
    exit 0
}

mkdir -p "$LOGDIR"
in_dvd=0
halt_respawn=0
fail_count=0
last_start_ts=0
claim_singleton
trap on_exit EXIT
trap on_signal TERM INT

log "daemon started"

while true; do
    now=$(date +%s 2>/dev/null || echo 0)

    if dvd_core_active; then
        if [ "$in_dvd" = "0" ]; then
            log "core entered DVD"
            in_dvd=1
            halt_respawn=0
            fail_count=0
            if start_launcher; then
                last_start_ts=$now
            else
                last_start_ts=$now
                fail_count=1
            fi
        elif ! launcher_running; then
            if [ "$halt_respawn" = "1" ]; then
                :
            else
                log "launcher exited unexpectedly"
                if [ "$last_start_ts" -gt 0 ] && \
                   [ $((now - last_start_ts)) -le "$RESPAWN_WINDOW_SEC" ]; then
                    fail_count=$((fail_count + 1))
                else
                    fail_count=1
                fi
                if [ "$fail_count" -gt "$RESPAWN_MAX" ]; then
                    log "respawn halted"
                    halt_respawn=1
                else
                    log "restart ${fail_count}/${RESPAWN_MAX}"
                    if start_launcher; then
                        last_start_ts=$now
                    fi
                fi
            fi
        fi
    else
        if [ "$in_dvd" = "1" ]; then
            log "core left DVD"
            in_dvd=0
            halt_respawn=0
            fail_count=0
            last_start_ts=0
            stop_launcher
        fi
    fi

    sleep "$POLL_SEC" 2>/dev/null || sleep 1
done
