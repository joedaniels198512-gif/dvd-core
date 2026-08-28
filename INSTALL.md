# Install MiSTer DVD Player (public beta 0.1)

No installer script is required. Extract the zip onto the SD card, then
**add** one `MiSTer.ini` section. Do not move ARM binaries after extract;
the tested layout is `/media/fat/DVD/dev/`.


## Manual install (GitHub zip)

### 1. Extract to the SD root

Copy **`MiSTer_DVD_Player_0.1.0-public-beta.zip`** to the MiSTer SD card
and extract its contents at the **root** of the FAT partition (`/media/fat`
when the system is running). You should have:

```
/media/fat/MiSTer_DVD_Player.rbf
/media/fat/MiSTer_DVD
/media/fat/DVD/dev/          launcher, player, ripping tools
/media/fat/DVD/lib/          libdvdread, libdvdnav, rip helper libraries
/media/fat/DVD/isos/         empty folder for your ISOs (optional)
```

Licence and corresponding-source files from the zip may sit beside those
paths; they are not required to run the player.

**Do not replace** stock `/media/fat/MiSTer` with `MiSTer_DVD`. Both files
must remain on the SD root. `MiSTer_DVD` is used only for this core.

If you already have a user-supplied `libdvdcss.so.2` under `DVD/lib`,
leave that file in place. This zip does not include it and extracting
should not be used as an excuse to delete the whole `DVD/lib` folder.


### 2. Edit MiSTer.ini (add, do not replace)

Open `/media/fat/MiSTer.ini` in a text editor.

**Add** this section. Do **not** overwrite or replace your existing
`MiSTer.ini` with a new file. Keep your other settings.

```
[DVD-Player]
main=MiSTer_DVD
```

Save the file. Reboot or return to the MiSTer menu so the new core
setting is picked up.


### 3. Launch DVD-Player

From the MiSTer main menu, start **MiSTer DVD Player**
(`MiSTer_DVD_Player.rbf`, core name `DVD-Player`).

MiSTer switches to `MiSTer_DVD` for this core only. The launcher shows:

- Play Physical DVD
- DVD Library
- Rip DVD to USB


## MiSTer Companion / Update All

Once this core is in a Companion / Update All database, those tools can
**inject** the same `[DVD-Player] main=MiSTer_DVD` entry automatically.
You still should not replace your whole `MiSTer.ini`. Until that database
support is available, add the block by hand as above.


## Play Physical DVD

1. Insert a DVD-Video disc in the SuperDock / optical drive (`/dev/sr0`).
2. Confirm **Play Physical DVD**.

PAL physical discs have been tested. **NTSC physical discs have not.**

CSS-encrypted commercial discs need user-supplied libdvdcss (below).
Unencrypted discs can play without it.


## ISO playback (DVD Library)

Put **DVD-Video `.iso` files** here:

| Location | What is scanned |
|---|---|
| `/media/fat/DVD/isos` | Files in that folder only |
| `/media/usbN/` | Files at the USB root |
| `/media/usbN/DVD` | Files plus one extra subdirectory |
| `/media/usbN/Movies` | Files plus one extra subdirectory |

`usbN` means `usb0`, `usb1`, … as MiSTer mounts them under `/media`.

Open **DVD Library**, pick a title, Confirm to play. NTSC ISO playback
has been tested. Files larger than 4 GB need a filesystem that allows
them (exFAT, not FAT32).

This is not a folder of loose `.VOB` files and not an MKV library.


## Rip DVD to USB

Use only discs you have the legal right to copy. See [LEGAL.md](LEGAL.md).

1. Insert the disc.
2. Plug in a writable USB stick (exFAT recommended).
3. Choose **Rip DVD to USB** and pick the USB volume.
4. Wait. Do not remove the disc or stick until it finishes.

The helper copies the disc with `dvdbackup`, then builds one DVD-Video
ISO with `genisoimage`. Output:

```
/media/usbN/DVD/<title>.iso
```

Temporary space needed is about **twice the disc size plus 512 MB**.
FAT32 cannot store an ISO larger than 4 GB; the ripper will refuse in
that case. If a file with the same name already exists, the rip stops.

Encrypted physical discs may fail to rip unless libdvdcss is present
(below). Unencrypted discs can rip without it.


## Encrypted discs (libdvdcss) — optional

**libdvdcss is not included with MiSTer DVD Player.** This project does
not ship it and does not download it.

The official upstream project is **VideoLAN libdvdcss**:

https://www.videolan.org/developers/libdvdcss.html

Do not download random binaries from unofficial mirrors. If you choose
to provide a compatible library, place:

```
/media/fat/DVD/lib/libdvdcss.so.2
```

Check the laws that apply to you. CSS and related rules vary by
jurisdiction.

Without that library:

- Encrypted **physical** discs may not play or rip
- Unencrypted discs remain supported
- ISO playback does not depend on this package distributing libdvdcss

An existing `libdvdcss.so.2` in that folder should be left unchanged.
Missing CSS is not an install failure.


## Uninstall (manual)

No uninstaller is shipped. To remove the core:

1. Delete `/media/fat/MiSTer_DVD_Player.rbf` and `/media/fat/MiSTer_DVD`
   (do **not** delete stock `/media/fat/MiSTer`).
2. Remove only the `[DVD-Player]` / `main=MiSTer_DVD` block you added
   from `MiSTer.ini`. Do not replace the whole ini file.
3. Optionally delete `/media/fat/DVD/` if you also want to remove ISOs,
   cache, logs, and a user-supplied `libdvdcss.so.2`.

If you previously used a marked `dvd_autostart_daemon` block in
`user-startup.sh`, delete that marked block. This beta does not need a
daemon.


## After install: where files live

| Path | Role |
|---|---|
| `/media/fat/MiSTer` | Stock MiSTer Main (untouched) |
| `/media/fat/MiSTer_DVD` | Core-specific Main for DVD-Player |
| `/media/fat/MiSTer_DVD_Player.rbf` | FPGA core |
| `/media/fat/DVD/dev/` | Launcher, player, rip tools |
| `/media/fat/DVD/lib/` | libdvdread, libdvdnav; optional libdvdcss |
| `/media/fat/DVD/isos/` | SD ISO library |
| `/media/fat/MiSTer.ini` | add `[DVD-Player] main=MiSTer_DVD` |
