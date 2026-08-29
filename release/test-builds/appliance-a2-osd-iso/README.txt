MiSTer DVD Player — Appliance A2 (OSD Play ISO)

Updates Appliance files only. The original launcher DVD Player is not replaced.

Existing (leave these alone):
  /media/fat/MiSTer_DVD_Player.rbf
  /media/fat/MiSTer_DVD
  /media/fat/DVD/

Appliance (this package may replace A1 Appliance files):
  /media/fat/DVD_Player_Appliance.rbf     CONF_STR DVD-Player-Appliance
  /media/fat/MiSTer_DVD_Appliance         OSD stays resident; forks dvd_appliance
  /media/fat/DVD_Appliance/{bin,lib,logs,config}

OSD: Play ISO... (F0) opens the standard MiSTer file browser for .iso files.
The selected Linux path is sent to the supervisor. The ISO is NOT uploaded
to FPGA memory.

------------------------------------------------------------------------------
Manual install (copy-safe)

  mkdir -p /media/fat/DVD_Appliance/{bin,lib,logs,config}

  cp DVD_Player_Appliance.rbf /media/fat/DVD_Player_Appliance.rbf
  cp MiSTer_DVD_Appliance /media/fat/MiSTer_DVD_Appliance
  cp dvd_appliance dvd_av_threaded_test /media/fat/DVD_Appliance/bin/
  cp -n lib/* /media/fat/DVD_Appliance/lib/
  chmod +x /media/fat/MiSTer_DVD_Appliance \
           /media/fat/DVD_Appliance/bin/dvd_appliance \
           /media/fat/DVD_Appliance/bin/dvd_av_threaded_test

  if ! grep -q '^\[DVD-Player-Appliance\]' /media/fat/MiSTer.ini; then
    printf '\n[DVD-Player-Appliance]\nmain=MiSTer_DVD_Appliance\n' >> /media/fat/MiSTer.ini
  fi

Do NOT copy into /media/fat/DVD.
Do NOT replace /media/fat/MiSTer_DVD or MiSTer_DVD_Player.rbf.

Rollback to A1: use release/test-builds/MiSTer_DVD_Player_appliance-a1-isolated.zip
and tag dvd-player-appliance-a1-known-good.

------------------------------------------------------------------------------
Runtime tests

A  Empty drive → insert physical DVD → autoplay. Eject → idle.
B  Idle → OSD Play ISO... → ISO under /media/fat → authored playback.
C  OSD Play ISO... → ISO under /media/usb0 → same playback, no copy to SD.
D  Hold CANCEL/B 3s during ISO → idle. Choose another ISO immediately.
E  Physical WAIT_EJECT (disc still in) → Play ISO → ISO plays.
   After ISO exit, inserted disc must not autoplay.
F  Original DVD Player core → launcher unchanged.

Logs: /media/fat/DVD_Appliance/logs/appliance.log
Look for: APPLIANCE OSD: ISO selected path=...
          APPLIANCE OSD: ISO path sent (no FPGA transfer)
          APPLIANCE: launching ISO path=...
