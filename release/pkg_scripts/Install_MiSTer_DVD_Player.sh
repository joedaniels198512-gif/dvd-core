#!/bin/sh
#
# MiSTer DVD Player 0.1.0-public-beta installer.
#
# Run from the MiSTer Scripts menu after extracting the ZIP to the SD root.
# Idempotent.
#
# Installs /media/fat/MiSTer_DVD beside stock /media/fat/MiSTer.
# Adds a marked [DVD-Player] main=MiSTer_DVD block to MiSTer.ini.
# Does NOT replace /media/fat/MiSTer.
# Migrates/removes the old marked user-startup daemon block.
# Does not start the autostart daemon (custom Main supervises the launcher).
# Preserves unrelated MiSTer.ini and user-startup contents.
# Refuses to overwrite a conflicting [DVD-Player] main= value.
#

set -e

VERSION="0.1.0-public-beta"
DVD_ROOT=/media/fat/DVD
DEVDIR=$DVD_ROOT/dev
BINDIR=$DVD_ROOT/bin
LIBDIR=$DVD_ROOT/lib
DATADIR=$DVD_ROOT/data
LOGDIR=$DVD_ROOT/logs
CACHEDIR=$DVD_ROOT/cache
ISOSDIR=$DVD_ROOT/isos
BACKUPDIR=$DVD_ROOT/backup
LAUNCHER=$DEVDIR/dvd_launcher
PLAYER=$DEVDIR/dvd_av_threaded_test
RIP=$DEVDIR/dvd_rip_iso
DAEMON_DST=$BINDIR/dvd_autostart_daemon.sh
RBF_DST=/media/fat/MiSTer_DVD_Player.rbf
MAIN_DST=/media/fat/MiSTer_DVD
INI=/media/fat/MiSTer.ini
STARTUP=/media/fat/linux/user-startup.sh
STARTUP_TEMPLATE=/media/fat/linux/_user-startup.sh
SCRIPTS_DIR=/media/fat/Scripts

MARKER_BEGIN="# MiSTer DVD Player auto-launch"
MARKER_END="# END MiSTer DVD Player auto-launch"
INI_BEGIN="# BEGIN MiSTer DVD Player main"
INI_END="# END MiSTer DVD Player main"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

die() {
    echo "ERROR: $*" >&2
    exit 1
}

info() {
    echo "$*"
}

same_file() {
    [ -f "$1" ] && [ -f "$2" ] || return 1
    cmp -s "$1" "$2" 2>/dev/null
}

backup_file() {
    _src=$1
    _name=$2
    [ -f "$_src" ] || return 0
    mkdir -p "$BACKUPDIR"
    if [ -f "$BACKUPDIR/$_name" ] && cmp -s "$_src" "$BACKUPDIR/$_name" 2>/dev/null; then
        return 0
    fi
    if [ -f "$BACKUPDIR/$_name" ]; then
        mv -f "$BACKUPDIR/$_name" "$BACKUPDIR/$_name.older" 2>/dev/null || true
    fi
    cp -f "$_src" "$BACKUPDIR/$_name"
    info "Backed up $_src -> $BACKUPDIR/$_name"
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

# Prints the [DVD-Player] main= value if present (exact section name).
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
    for p in \
        "$HERE/../MiSTer_DVD_Player.rbf" \
        /media/fat/MiSTer_DVD_Player.rbf \
        "$DVD_ROOT/MiSTer_DVD_Player.rbf" \
        "$DEVDIR/MiSTer_DVD_Player.rbf"
    do
        if [ -f "$p" ]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

find_main() {
    for p in \
        "$HERE/../MiSTer_DVD" \
        "$DVD_ROOT/dev/MiSTer_DVD" \
        /media/fat/MiSTer_DVD
    do
        if [ -f "$p" ]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

info "MiSTer DVD Player installer"
info "Version: $VERSION"
info "==========================="

[ -f "$LAUNCHER" ] || die "missing launcher: $LAUNCHER"
[ -f "$PLAYER" ] || die "missing player: $PLAYER"
[ -d "$LIBDIR" ] || die "missing library dir: $LIBDIR"
ls "$LIBDIR"/libdvdread.so* >/dev/null 2>&1 || \
    die "missing libdvdread in $LIBDIR"
ls "$LIBDIR"/libdvdnav.so* >/dev/null 2>&1 || \
    die "missing libdvdnav in $LIBDIR"
# libdvdcss is optional and is not distributed with this package.
# Do not fail if it is absent. Do not download it. Do not delete or overwrite it.

MAIN_SRC=$(find_main) || die "missing MiSTer_DVD (install beside stock MiSTer; do not replace /media/fat/MiSTer)"

mkdir -p "$BINDIR" "$DEVDIR" "$LIBDIR" "$DATADIR" "$LOGDIR" "$CACHEDIR" \
         "$ISOSDIR" "$BACKUPDIR" "$SCRIPTS_DIR" /media/fat/linux \
         /media/fat/games/DVD-Player

# Keep daemon source on disk; custom Main no longer starts it.
if [ ! -f "$DAEMON_DST" ] && [ -f "$HERE/../DVD/bin/dvd_autostart_daemon.sh" ]; then
    cp -f "$HERE/../DVD/bin/dvd_autostart_daemon.sh" "$DAEMON_DST"
fi

chmod +x "$LAUNCHER" "$PLAYER" 2>/dev/null || true
[ -f "$DAEMON_DST" ] && chmod +x "$DAEMON_DST" 2>/dev/null || true
[ -f "$RIP" ] && chmod +x "$RIP" 2>/dev/null || true
[ -f "$DEVDIR/dvdbackup" ] && chmod +x "$DEVDIR/dvdbackup" 2>/dev/null || true
[ -f "$DEVDIR/genisoimage" ] && chmod +x "$DEVDIR/genisoimage" 2>/dev/null || true

FOUND=$(find_rbf) || die "MiSTer_DVD_Player.rbf not found"
info "RBF source: $FOUND"
if [ "$FOUND" = "$RBF_DST" ]; then
    info "RBF already at $RBF_DST"
else
    if [ -f "$RBF_DST" ] && ! same_file "$FOUND" "$RBF_DST"; then
        backup_file "$RBF_DST" "MiSTer_DVD_Player.rbf"
    fi
    cp -f "$FOUND" "$RBF_DST"
    info "Installed RBF: $RBF_DST"
fi

backup_file "$LAUNCHER" "dvd_launcher"
backup_file "$PLAYER" "dvd_av_threaded_test"

if [ "$MAIN_SRC" = "$MAIN_DST" ]; then
    info "MiSTer_DVD already at $MAIN_DST"
else
    if [ -f "$MAIN_DST" ] && ! same_file "$MAIN_SRC" "$MAIN_DST"; then
        backup_file "$MAIN_DST" "MiSTer_DVD"
    fi
    cp -f "$MAIN_SRC" "$MAIN_DST"
    info "Installed $MAIN_DST (stock /media/fat/MiSTer was not replaced)"
fi
chmod +x "$MAIN_DST" 2>/dev/null || true

if [ -f /media/fat/MiSTer ]; then
    if cmp -s /media/fat/MiSTer "$MAIN_DST" 2>/dev/null; then
        die "refusing to proceed: /media/fat/MiSTer is identical to MiSTer_DVD; stock Main must remain separate"
    fi
    info "Stock Main left in place: /media/fat/MiSTer"
fi

# --- MiSTer.ini [DVD-Player] main=MiSTer_DVD ---
EXISTING_MAIN=""
if [ -f "$INI" ]; then
    EXISTING_MAIN=$(dvd_ini_main "$INI" || true)
fi
if [ -n "$EXISTING_MAIN" ] && [ "$EXISTING_MAIN" != "MiSTer_DVD" ]; then
    die "conflicting $INI [DVD-Player] main=$EXISTING_MAIN (expected MiSTer_DVD). Not overwriting a custom Main setting."
fi

if [ ! -f "$INI" ]; then
    printf '%s\n' "$INI_BEGIN" "[DVD-Player]" "main=MiSTer_DVD" "$INI_END" > "$INI"
    info "Created $INI with [DVD-Player] main=MiSTer_DVD"
elif grep -F -q "$INI_BEGIN" "$INI"; then
    # Rewrite the marked block so a previous [DVD] main= install becomes [DVD-Player].
    backup_file "$INI" "MiSTer.ini"
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
    backup_file "$INI" "MiSTer.ini"
    {
        echo ""
        echo "$INI_BEGIN"
        echo "[DVD-Player]"
        echo "main=MiSTer_DVD"
        echo "$INI_END"
    } >> "$INI"
    info "Appended [DVD-Player] main=MiSTer_DVD to $INI"
fi

# --- migrate off the user-startup daemon ---
if [ ! -f "$STARTUP" ]; then
    if [ -f "$STARTUP_TEMPLATE" ]; then
        cp "$STARTUP_TEMPLATE" "$STARTUP"
        info "Created $STARTUP from template"
    else
        printf '%s\n' '#!/bin/sh' 'echo "***" $1 "***"' > "$STARTUP"
        info "Created new $STARTUP"
    fi
fi
chmod +x "$STARTUP" 2>/dev/null || true

if [ -f "$STARTUP" ] && grep -F -q "$MARKER_BEGIN" "$STARTUP"; then
    strip_marked_block "$STARTUP" "$MARKER_BEGIN" "$MARKER_END"
    info "Removed marked DVD daemon block from $STARTUP (custom Main replaces it)"
else
    info "No marked DVD daemon block in user-startup.sh"
fi

stop_matching "dvd_autostart_daemon"
info "Stopped leftover dvd_autostart_daemon if it was running"
info "Daemon source preserved at $DAEMON_DST (not started)"

for s in Install_MiSTer_DVD_Player.sh Uninstall_MiSTer_DVD_Player.sh \
         Export_MiSTer_DVD_Player_Logs.sh
do
    if [ -f "$HERE/$s" ]; then
        chmod +x "$HERE/$s" 2>/dev/null || true
        if [ "$HERE" != "$SCRIPTS_DIR" ] && [ -d "$SCRIPTS_DIR" ]; then
            if [ ! -f "$SCRIPTS_DIR/$s" ] || ! same_file "$HERE/$s" "$SCRIPTS_DIR/$s"; then
                cp -f "$HERE/$s" "$SCRIPTS_DIR/$s"
            fi
            chmod +x "$SCRIPTS_DIR/$s" 2>/dev/null || true
        fi
    fi
done

info ""
info "Install complete."
info "Stock Main: /media/fat/MiSTer"
info "DVD Main:   /media/fat/MiSTer_DVD"
info "INI:        [DVD-Player] main=MiSTer_DVD"
info "Games dir:  /media/fat/games/DVD-Player"
info "Main menu item: MiSTer DVD Player"
info "Re-running this installer is safe and will not duplicate startup or INI entries."
info "Library cache and logs were preserved."
info ""
info "libdvdcss is not distributed with MiSTer DVD Player."
info "Encrypted physical DVD playback and ripping require a compatible"
info "user-supplied libdvdcss.so.2 at /media/fat/DVD/lib/libdvdcss.so.2"
info "Consult the official VideoLAN libdvdcss project and applicable local law."
if [ -e "$LIBDIR/libdvdcss.so.2" ] || [ -e "$LIBDIR/libdvdcss.so.2.2.0" ]; then
    info "An existing libdvdcss library was found and was left unchanged."
else
    info "No libdvdcss.so.2 found; unencrypted discs and ISO playback remain available."
    info "Encrypted physical DVD playback/ripping may be unavailable until CSS is supplied."
fi
