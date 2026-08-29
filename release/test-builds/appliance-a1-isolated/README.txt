MiSTer DVD Player — Appliance A1 isolated (true parallel core)

This package installs ALONGSIDE the existing launcher DVD Player.
It does not replace any file of the pinned known-good installation.

Existing (leave these alone; do not copy over them):
  /media/fat/MiSTer_DVD_Player.rbf
  /media/fat/MiSTer_DVD
  /media/fat/DVD/

New files only:
  /media/fat/DVD_Player_Appliance.rbf
  /media/fat/MiSTer_DVD_Appliance
  /media/fat/DVD_Appliance/{bin,lib,logs,config}

FPGA identity:
  CONF_STR "DVD-Player-Appliance"  →  MiSTer.ini [DVD-Player-Appliance]
  main=MiSTer_DVD_Appliance        →  /media/fat/DVD_Appliance/bin/dvd_appliance

The original mapping is unchanged:
  CONF_STR "DVD-Player"            →  MiSTer.ini [DVD-Player]
  main=MiSTer_DVD                  →  /media/fat/DVD/dev/dvd_launcher

No shared-Main filename dispatch. The old Main is not replaced.
No rollback of the old core is required because it is not modified.

------------------------------------------------------------------------------
Manual install (copy-safe; no installer)

On the SS1 (or from a mounted SD card), from this unzipped package:

  mkdir -p /media/fat/DVD_Appliance/bin \
           /media/fat/DVD_Appliance/lib \
           /media/fat/DVD_Appliance/logs \
           /media/fat/DVD_Appliance/config

  cp -n DVD_Player_Appliance.rbf /media/fat/DVD_Player_Appliance.rbf
  cp -n MiSTer_DVD_Appliance /media/fat/MiSTer_DVD_Appliance
  cp dvd_appliance /media/fat/DVD_Appliance/bin/dvd_appliance
  cp dvd_av_threaded_test /media/fat/DVD_Appliance/bin/dvd_av_threaded_test
  cp -n lib/* /media/fat/DVD_Appliance/lib/
  chmod +x /media/fat/MiSTer_DVD_Appliance \
           /media/fat/DVD_Appliance/bin/dvd_appliance \
           /media/fat/DVD_Appliance/bin/dvd_av_threaded_test

  # Register the new Main. Do not edit the existing [DVD-Player] block.
  if ! grep -q '^\[DVD-Player-Appliance\]' /media/fat/MiSTer.ini; then
    printf '\n[DVD-Player-Appliance]\nmain=MiSTer_DVD_Appliance\n' >> /media/fat/MiSTer.ini
  fi

Do NOT copy anything into /media/fat/DVD.
Do NOT replace /media/fat/MiSTer_DVD.
Do NOT replace /media/fat/MiSTer_DVD_Player.rbf.

If physical CSS discs fail to open, copy libdvdcss from the existing
/media/fat/DVD/lib into /media/fat/DVD_Appliance/lib (read of the old
tree only). The supervisor also searches /media/fat/DVD/lib at runtime
and never writes there.

------------------------------------------------------------------------------
Menu names

  MiSTer DVD Player          → existing launcher/library/ripper
  DVD Player Appliance       → insert disc, autoplay (this package)

------------------------------------------------------------------------------
Acceptance

1. Old core: launcher and ISO library unchanged. Works with Appliance
   files absent or present.
2. Appliance + disc already in: no launcher; authored playback starts.
3. Appliance + empty drive: idle; insert disc; playback starts.
4. Hold CANCEL/B 3s: playback exits; same disc does not restart.
5. Eject then insert: autoplay rearms and starts again.
6. Audio/data CD: logged as unsupported; player is not launched.

Logs:
  /media/fat/DVD_Appliance/logs/appliance.log
  /media/fat/DVD_Appliance/logs/player.log

------------------------------------------------------------------------------
Remove Appliance only (old core stays)

  rm -f /media/fat/DVD_Player_Appliance.rbf
  rm -f /media/fat/MiSTer_DVD_Appliance
  rm -rf /media/fat/DVD_Appliance
  # Optionally delete the [DVD-Player-Appliance] block from MiSTer.ini
