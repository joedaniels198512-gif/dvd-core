# MiSTer DVD Player

**Public beta 0.1** for SuperStation One and compatible MiSTer hardware with
an optical drive.

This is an experimental DVD-Video player. It is not a finished commercial
DVD appliance, and it is not a general video-file player (no MKV, H.264, or
similar).

by Mojojojo198512


## What it does

- Play a **physical DVD** from a compatible optical drive (SuperDock or
  similar, seen by Linux as `/dev/sr0`)
- Play **DVD-Video ISO** files from the SD card or USB
- Follow **authored DVD menus** and navigation
- Output over **HDMI** (MiSTer scaler) and **native CRT**
- Support **PAL** and **NTSC** video (see testing status below)
- **Rip a physical DVD to a USB stick** as a single `.iso`

When you select the DVD-Player core, MiSTer switches to a core-specific
Main binary (`MiSTer_DVD`) that starts the launcher automatically. Stock
`/media/fat/MiSTer` is not replaced. No background daemon is required.


## Testing status (this beta)

| Path | Status |
|---|---|
| PAL physical disc | Tested successfully |
| NTSC ISO | Tested |
| NTSC physical disc | **Not tested** |

Settled playback samples on the freeze hardware:

- PAL physical feature: **24.99 fps** vs 25.00 nominal, **0** new underruns
- NTSC ISO: **29.95 fps** vs 29.97 nominal, **0** new underruns


## Hardware

Verified on **SuperStation One** (Cyclone V, dual-core Cortex-A9) with a
working optical drive at `/dev/sr0`.

You need:

- SuperStation One or compatible MiSTer
- A compatible USB optical drive / SuperDock that appears as `/dev/sr0`
- HDMI and/or CRT as supported by your MiSTer video setup
- For ripping: a writable USB volume mounted as `/media/usb0`,
  `/media/usb1`, … (exFAT recommended for large discs)


## Install

See **[INSTALL.md](INSTALL.md)**.


## Basic controls

Launcher (D-pad + Confirm / Back):

1. **Play Physical DVD**
2. **DVD Library** (ISOs)
3. **Rip DVD to USB**

During playback (MiSTer joystick mapping for this core):

| Control | Action |
|---|---|
| D-pad | Menu / highlight navigation |
| Confirm (A) | Select |
| Back (B) | DVD back / cancel |
| Hold Back ~3 seconds | Return to the launcher |
| Start | Play / Pause |
| DVD Menu (X) | DVD menu |
| L / R | Previous / next chapter |
| Y | Subtitle toggle (where the disc supports it) |
| Select | Next audio track |

OSD (usual MiSTer OSD button): **TV Mode** (Auto / NTSC / PAL),
**CRT** (Native / Stabilized), **A/V Sync**. HDMI users often leave CRT on
Native. Stabilized is intended mainly for CRT.


## CSS-encrypted discs

**This package does not include libdvdcss.**

Unencrypted discs and ISO playback do not depend on us distributing that
library. CSS-encrypted **physical** discs may not play or rip until you
place a compatible `libdvdcss.so.2` yourself. Details: [INSTALL.md](INSTALL.md)
and [LEGAL.md](LEGAL.md).


## Known limitations

See **[KNOWN_ISSUES.md](KNOWN_ISSUES.md)**. In short: public beta; NTSC
physical discs untested; encrypted physical discs need user-supplied
libdvdcss; ripping needs spare USB space; startup can briefly glitch
while audio becomes the clock master.


## Licence and source

Open-source community project. See [LEGAL.md](LEGAL.md),
[LICENSING.md](LICENSING.md), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
[SOURCE_INFO.md](SOURCE_INFO.md), and [LICENSES/](LICENSES/).

Corresponding source for redistributed Debian binaries and FFmpeg is in
the public-beta zip under `SOURCES/` and in this repository.


## Credits

[CREDITS.md](CREDITS.md)


## Screenshots

_Add screenshots here when available._


## Other documents

- [INSTALL.md](INSTALL.md) — install, use, uninstall
- [RELEASE_NOTES_0.1.md](RELEASE_NOTES_0.1.md) — 0.1 announcement
- [CHANGELOG.md](CHANGELOG.md)
- [PRIVACY.md](PRIVACY.md)
- [COMPLIANCE_PUBLIC_BETA_0.1.md](COMPLIANCE_PUBLIC_BETA_0.1.md) —
  package contents and licence compliance (technical)
