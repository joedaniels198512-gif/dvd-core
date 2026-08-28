# MiSTer DVD Player 0.1.0-beta

**Public beta** for SuperStation One / MiSTer with an optical drive.

Experimental DVD-Video player: physical discs, ISOs, authored menus,
HDMI and CRT, and DVD ripping to USB. Not a finished commercial appliance.
Stock MiSTer Main is not replaced.

**Download:** `MiSTer_DVD_Player_0.1.0-public-beta.zip`  
**Install:** [INSTALL.md](INSTALL.md)


## Highlights

- Physical DVD playback from a compatible drive / SuperDock (`/dev/sr0`)
- PAL and NTSC (see testing notes)
- Authored DVD menus and navigation
- HDMI (MiSTer scaler) and native CRT output
- DVD-Video ISO playback from SD or USB
- Rip DVD to USB as a single `.iso`
- Daemon-free launch: `[DVD-Player] main=MiSTer_DVD` starts the launcher
  with the core
- CSS-encrypted **physical** discs only if **you** supply compatible
  VideoLAN **libdvdcss** — **not included** in this zip


## Testing

- PAL physical feature playback: tested
- NTSC ISO: tested
- NTSC physical disc: **not tested**

Settled samples on the freeze hardware:

- PAL physical feature: **24.99 fps** vs 25.00 nominal, **0** new underruns
- NTSC ISO: **29.95 fps** vs 29.97 nominal, **0** new underruns


## libdvdcss

This release does **not** ship `libdvdcss`. Official project:

https://www.videolan.org/developers/libdvdcss.html

Check laws that apply to you. Unencrypted discs and ISOs do not depend
on this zip containing CSS.


## Licence

Open-source components and corresponding source: [LEGAL.md](LEGAL.md),
[LICENSING.md](LICENSING.md), zip `SOURCES/` and `LICENSES/`.


## Known limitations

[KNOWN_ISSUES.md](KNOWN_ISSUES.md)
