MiSTer DVD Player
Version 0.1.0-private-beta
by Mojojojo198512

PRIVATE BETA — invited hardware testers only.
Not a public release.


WHAT IT IS
==========

MiSTer DVD Player is an experimental DVD-Video player for SuperStation One
and compatible MiSTer hardware with an optical drive (SuperDock or similar).

This beta plays physical DVDs (including CSS-encrypted commercial discs)
and DVD-Video ISO files from SD or USB. It is not a general media player.


INSTALL
=======

1. Extract this ZIP onto the ROOT of the MiSTer SD card so you have:

       /media/fat/MiSTer_DVD_Player.rbf
       /media/fat/MiSTer_DVD
       /media/fat/DVD/
       /media/fat/Scripts/Install_MiSTer_DVD_Player.sh

2. Boot MiSTer.

3. Scripts -> Install MiSTer DVD Player

4. Main menu -> MiSTer DVD Player

The installer is safe to run more than once. It installs MiSTer_DVD
beside stock /media/fat/MiSTer (the stock binary is never replaced) and
adds a marked [DVD-Player] main=MiSTer_DVD block to MiSTer.ini. It will refuse
if [DVD-Player] already has a different main= value. Any previous marked
user-startup daemon block is removed; the daemon source is kept under
DVD/bin/ but is not started.

This private beta keeps ARM binaries in:

    /media/fat/DVD/dev/

That is the tested layout. Do not move them to DVD/bin/ by hand.

MiSTer games folder for this core (CONF_STR name) is:

    /media/fat/games/DVD-Player


UNINSTALL
=========

Scripts -> Uninstall MiSTer DVD Player

That stops the player/launcher, removes the marked autostart and
[DVD-Player] main= blocks, and removes MiSTer_DVD_Player.rbf plus /media/fat/MiSTer_DVD.
Stock /media/fat/MiSTer is not touched. /media/fat/DVD/ (ISOs, cache, logs,
daemon source) stays. Delete that folder on the SD card only if you want a
full wipe.


BASIC CONTROLS
==============

    D-pad              Navigate DVD menus
    Confirm            Select
    BACK               Back / menu back
    Hold BACK 3 sec    Exit DVD to the launcher
    Start              Play / Pause
    Previous / Next    Chapter skip
    PREVIOUS + NEXT    Audio Next  (PRIVATE BETA TEMPORARY)

Hold Previous and Next together to cycle authored audio tracks
(languages / commentary).

The final build will bind WEST / Y to Audio Next once the FPGA exposes
that button to ARM. Do not expect WEST / Y to change audio in this beta.


ON-SCREEN DISPLAY (OSD)
=======================

Opened with the usual MiSTer OSD button.

    TV Mode          Auto / NTSC / PAL
    CRT Stabilizer   On / Off
    A/V Sync         delay in 10 ms steps

A/V Sync (important for this beta)
----------------------------------
This FPGA/player build has a known calibration issue.

    If lip-sync looks slightly early or late, try A/V Sync +20 ms first.

+20 ms approximately matches previous known-good timing.
The intended final behaviour is: A/V Sync 0 ms = calibrated baseline,
with 5 ms steps. That is NOT this beta (it would need a new FPGA build).

CRT Stabilizer
--------------
Intended mainly for CRT output.
HDTV / HDMI users may prefer it Off.


DVD LIBRARY
===========

Put DVD-Video .iso files in:

    SD:   /media/fat/DVD/isos
    USB:  /media/usb*/DVD   or  /media/usb*/Movies

Files larger than 4 GB are supported on exFAT (and other filesystems
that allow large files). FAT32 cannot store a >4 GB ISO.


RIP DVD TO USB — EXPERIMENTAL
=============================

This beta can decrypt a physical DVD to a single .iso on USB.

    Destination:  /media/usb*/DVD/<title>.iso
    Recommended USB filesystem:  exFAT
    FAT32 cannot store ISOs larger than 4 GB
    Temporary space needed: about twice the disc size + ~512 MB
    Do not remove the DVD or USB drive during a rip

The first hardware rip had started successfully when this package was
built, but had not yet been fully validated through completion.
Treat ripping as experimental. The finished product is one .iso — not
a VIDEO_TS folder.

Use only media you are authorised to copy.


WHAT THIS BETA DOES NOT INCLUDE
===============================

- Movie subtitles (authored menu highlights/icons ARE implemented)
- Multi-angle (planned for a later version)
- MKV / H.264 / Xvid / general video-file playback
- WEST / Y as Audio Next (temporary PREV+NEXT chord instead)


NEON GENESIS EVANGELION
=======================

Do not treat the Evangelion Communications-page navigation issue as
generally fixed. That disc has historically shown unusual navigation.

Audio-track logical/physical mapping HAS been fixed; Japanese audio
switching now works.


BUG REPORTS (no SSH required)
=============================

1. Stop after reproducing the issue once if you can.
2. Scripts -> Export MiSTer DVD Player Logs
3. Send:

       the exported archive from the SD card root
       DVD title
       exact menu / action immediately before the failure
       CRT or HDMI
       physical disc or ISO

Logs (on the SD card)
---------------------
    /media/fat/DVD/logs/launcher.log           launcher + player output
    /media/fat/DVD/logs/launcher.previous.log  previous launcher session
    /media/fat/DVD/logs/autostart.log          leftover from older daemon installs
    /media/fat/DVD/logs/rip.log                rip helper
    /media/fat/DVD/logs/rip.previous.log
    /media/fat/DVD/logs/rip.child.log          dvdbackup / genisoimage text

Player output is currently inherited into launcher.log (MiSTer_DVD
redirects the launcher, and the launcher starts the player as a child).
There is no separate player.log in this beta.


LEGAL
=====

Use only media you are authorised to access or copy.

This package redistributes GPL/LGPL components (dvdbackup, genisoimage,
libdvdread, libdvdnav, libdvdcss, FFmpeg, libmagic, zlib, bzip2).
See LICENSES/ and SOURCES/ in this ZIP.
