# MiSTer DVD Player

## ⬇️ Download

### [Download MiSTer DVD Player v0.2.0 Beta 2](https://github.com/joedaniels198512-gif/dvd-core/releases/download/v0.2.0-beta.2/MiSTer_DVD_Player_0.2.0-beta.2.zip)

**Do not use GitHub's "Code → Download ZIP" button.**
That downloads the source repository, not the ready-to-install DVD Player package.

Extract `MiSTer_DVD_Player_0.2.0-beta.2.zip` to the root of your MiSTer SD card.

> ## Encrypted commercial DVDs / libdvdcss

`libdvdcss` is **not included** with MiSTer DVD Player.

Many commercial DVDs use CSS encryption. To play these discs, you may need to provide your own copy of:

`libdvdcss.so.2`

The MiSTer / SuperStation One requires a **32-bit ARM hard-float Linux (`armhf`)** build. A Windows, macOS, x86 Linux, or ARM64 build will not work.

### Where to get libdvdcss

The Archives of the internet might have it somewhere if you search for exactly libdvdcss.so.2

### Installing it for MiSTer DVD Player

Place the library here:

`/media/fat/DVD/lib/libdvdcss.so.2`

If your build produces a versioned file such as:

`libdvdcss.so.2.x.x`

copy the versioned file into the same directory and create a symlink named `libdvdcss.so.2`.

For example:

```bash
cp libdvdcss.so.2.x.x /media/fat/DVD/lib/
cd /media/fat/DVD/lib
ln -sf libdvdcss.so.2.x.x libdvdcss.so.2
```

The important path that DVD Player must be able to open is:

`/media/fat/DVD/lib/libdvdcss.so.2`

### DVD Ripper

For the separate DVD Ripper utility, place it here instead:

`/media/fat/DVD-Ripper/lib/libdvdcss.so.2`

### Important

MiSTer DVD Player does not distribute, download, or install libdvdcss.

Please check the laws that apply where you live before using software that circumvents DVD copy protection.

**v0.2.0 beta 2** for SuperStation One and compatible MiSTer hardware with
an optical drive.

A DVD-Video player for MiSTer / SuperStation One. It is beta software, not
a finished commercial DVD player, and not a general video-file player
(no MKV, H.264, or similar).


## What it is

Select **DVD Player** on MiSTer. A physical DVD already in the drive
autoplays. Insert a disc while the core is open and it autoplays. Use
OSD **Play ISO...** for DVD-Video ISO files from USB or SD.


## Current primary features

- Physical DVD playback from a compatible optical drive (`/dev/sr0`)
- Physical disc autoplay inside the DVD Player core
- Authored DVD navigation and menus
- ISO playback from OSD **Play ISO...**
- USB and SD ISO storage (USB wins when the core folder exists)
- PAL and NTSC
- Automatic 4:3 / 16:9 on HDMI / scaler (authored DVD IFO)
- Manual aspect override (OSD Auto / 4:3 / 16:9)
- Subtitles
- Runtime audio-track switching
- Interactive DVD stills and menus

## Can it play encrypted DVDs and ISO?
Encrypted commercial DVDs: libdvdcss is not included with this project. You must source your own copy of
libdvdcss.so.2 and place it at /media/fat/DVD/lib/libdvdcss.so.2. to play DVDs and ISO directly


## Install

See **[INSTALL.md](INSTALL.md)**.

Extract **`MiSTer_DVD_Player_0.2.0-beta.2.zip`** to the **root** of the
MiSTer SD card (`/media/fat` when running), then **add** (do not replace
your existing `MiSTer.ini`):

```
[DVD-Player]
main=MiSTer_DVD
```

You should have:

```
/media/fat/DVD_Player.rbf
/media/fat/MiSTer_DVD
/media/fat/DVD/bin/
/media/fat/DVD/lib/
/media/fat/DVD/logs/
/media/fat/DVD/config/
/media/fat/games/DVD-Player/
```

Do **not** replace stock `/media/fat/MiSTer` with `MiSTer_DVD`.


## ISO storage

- SD (created automatically): `/media/fat/games/DVD-Player/`
- USB (create this folder yourself if you want USB ISOs):
  `/games/DVD-Player/` on the USB drive  
  Example: `/media/usb0/games/DVD-Player/Movie.iso`

If the USB core folder exists, MiSTer uses it. Otherwise the SD folder
is used. Empty USB core folders are not created automatically. Existing
ISOs are never moved or copied for you.


## Physical drive

Verified on **SuperStation One** (Cyclone V, dual-core Cortex-A9) with a
USB optical drive / SuperDock that Linux sees as `/dev/sr0`.

You need:

- SuperStation One or compatible MiSTer
- A compatible USB optical drive / SuperDock at `/dev/sr0`
- HDMI and/or CRT as supported by your MiSTer video setup


## Physical DVD

- Select DVD Player — an already inserted DVD autoplays
- Insert a DVD while the core is open — it autoplays
- Eject — playback stops
- Hold CANCEL/B — stops playback
- The same physical disc does not instantly restart until eject/reinsert


## ISO playback

OSD **Play ISO...** opens the standard MiSTer file browser (USB-first
core folder, then SD). The full path is passed to ARM. The ISO is not
streamed to the FPGA. ISO → ISO and physical DVD → ISO replacement are
supported, including filenames with spaces.


## Aspect

OSD **Aspect: Auto / 4:3 / 16:9**, default **Auto**.

Authored DVD IFO `display_aspect_ratio`: `0` = 4:3, `3` = 16:9.
HDMI / scaler follows that automatically. The native analogue CRT raster
is not stretched; a widescreen TV may need a manual 16:9 setting.


## Controls

During playback (this core’s joystick mapping):

| Control | Action |
|---|---|
| D-pad | Menu / highlight navigation |
| Confirm (A) | Select |
| Back (B) | DVD back / cancel |
| Hold Back ~3 seconds | Stop playback |
| Start | Play / Pause |
| DVD Menu (X) | DVD menu |
| L / R | Previous / next chapter |
| Y | Subtitle toggle (where the disc supports it) |
| Select | Next audio track |

OSD also has **TV Mode** (Auto / NTSC / PAL), **CRT** (Native /
Stabilized), and **A/V Sync**. HDMI users usually leave CRT on Native.


## Logs

`/media/fat/DVD/logs/player.log`  
`/media/fat/DVD/logs/dvd_player.log`


## CSS-encrypted discs

**This package does not include libdvdcss.**

Unencrypted discs and ISO playback do not depend on us distributing that
library. Encrypted commercial **physical** discs require you to obtain
and install libdvdcss separately. The runtime file must be named exactly
`libdvdcss.so.2` at `/media/fat/DVD/lib/libdvdcss.so.2`. Details:
[INSTALL.md](INSTALL.md) and [LEGAL.md](LEGAL.md).


## Limitations

See **[KNOWN_ISSUES.md](KNOWN_ISSUES.md)**. In short: this is beta
software; not every DVD is guaranteed to work; encrypted physical discs
need user-supplied libdvdcss; native CRT does not signal 4:3 vs 16:9 to
the television; multi-angle switching is not implemented.


## Compatibility reports

Please use the GitHub **DVD compatibility report** issue form. Do **not**
upload commercial DVD ISOs, VOBs, decrypted disc contents, or CSS keys.


## Licence and source

Open-source community project. See [LEGAL.md](LEGAL.md),
[LICENSING.md](LICENSING.md), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
[SOURCE_INFO.md](SOURCE_INFO.md), and [LICENSES/](LICENSES/).

Corresponding source for redistributed Debian binaries and FFmpeg is in
the zip under `SOURCES/` and in this repository.


## Credits

[CREDITS.md](CREDITS.md)


## Other documents

- [INSTALL.md](INSTALL.md)
- [CHANGELOG.md](CHANGELOG.md)
- [COMPATIBILITY.md](COMPATIBILITY.md)
- [CONTRIBUTING.md](CONTRIBUTING.md)
- [PRIVACY.md](PRIVACY.md)
