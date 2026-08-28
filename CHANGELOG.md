# Changelog

## 0.1.0-beta

First **public** beta of MiSTer DVD Player for SuperStation One / MiSTer.

### Features

- DVD-Player FPGA core (`MiSTer_DVD_Player.rbf`) at native DVD resolution
  with HDMI scaler output and native CRT
- Core-specific Main (`MiSTer_DVD`) via `[DVD-Player] main=MiSTer_DVD`;
  stock `/media/fat/MiSTer` unchanged; no autostart daemon
- Launcher: Play Physical DVD, DVD Library, Rip DVD to USB
- Player: DVD-Video from `/dev/sr0` or ISO, authored menus/navigation,
  PAL and NTSC timing
- ISO library scan: `/media/fat/DVD/isos` and `/media/usbN` (`DVD` /
  `Movies` subfolders)
- Ripping: `dvdbackup` + `genisoimage` → `/media/usbN/DVD/<title>.iso`
- Optional user-supplied `/media/fat/DVD/lib/libdvdcss.so.2` (not shipped)

### Beta limitations

- NTSC physical discs not tested (NTSC ISO tested; PAL physical tested)
- Encrypted physical play/rip needs user-supplied libdvdcss
- Startup may briefly drop/correct frames before audio master clock
- Ripping needs roughly 2× disc size + 512 MB on USB; FAT32 4 GB limit
- Not a general media player

### Package

Conservative public zip: extract to SD root; no installer script.
Add `[DVD-Player] main=MiSTer_DVD` to `MiSTer.ini` (Companion / Update All
can inject that later). No libdvdcss binary. Licence texts and Debian /
FFmpeg corresponding source included. Frozen binaries: see
[COMPLIANCE_PUBLIC_BETA_0.1.md](COMPLIANCE_PUBLIC_BETA_0.1.md) and
[RELEASE_CANDIDATE_0.1.md](RELEASE_CANDIDATE_0.1.md).
