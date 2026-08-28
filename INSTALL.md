# Install MiSTer DVD Player (public beta 0.1)

These steps match the current public-beta zip and
`Scripts/Install_MiSTer_DVD_Player.sh`. Do not move ARM binaries by hand
after extract; the tested layout is `/media/fat/DVD/dev/`.


## 1. Extract the zip

Copy **`MiSTer_DVD_Player_0.1.0-public-beta.zip`** onto the MiSTer SD card
and extract it at the **root** of the FAT partition so you have:

```
/media/fat/MiSTer_DVD_Player.rbf
/media/fat/MiSTer_DVD
/media/fat/DVD/dev/          launcher, player, ripping tools
/media/fat/DVD/lib/          libdvdread, libdvdnav, rip helper libraries
/media/fat/DVD/isos/         empty folder for your ISOs (optional)
/media/fat/Scripts/Install_MiSTer_DVD_Player.sh
/media/fat/Scripts/Uninstall_MiSTer_DVD_Player.sh
```

Licence and corresponding-source files from the zip may sit beside those
paths; they are not required to run the player.


## 2. Run the installer

Boot MiSTer. Open **Scripts → Install MiSTer DVD Player**.

The installer is safe to run more than once. It will:

- Copy `MiSTer_DVD_Player.rbf` to `/media/fat/MiSTer_DVD_Player.rbf` if needed
- Install **`/media/fat/MiSTer_DVD` beside** stock **`/media/fat/MiSTer`**
  (stock Main is never replaced)
- Add a marked block to `MiSTer.ini`:

  ```
  [DVD-Player]
  main=MiSTer_DVD
  ```

- Create `/media/fat/games/DVD-Player` if missing
- Remove any **old marked daemon block** from
  `/media/fat/linux/user-startup.sh` and stop a leftover
  `dvd_autostart_daemon` if it was running
- Leave library cache, logs, and an existing `libdvdcss` file untouched

It **refuses** if `[DVD-Player]` already has a `main=` value other than
`MiSTer_DVD`. It **refuses** if `/media/fat/MiSTer` is identical to
`MiSTer_DVD` (stock Main must stay a separate file).

A daemon is **not** required. Custom Main starts the launcher when you
load this core.


## 3. Launch DVD-Player

From the MiSTer main menu, start **MiSTer DVD Player**
(`MiSTer_DVD_Player.rbf`, core name `DVD-Player`).

MiSTer switches to `MiSTer_DVD` for this core only. The launcher appears
with three items:

- Play Physical DVD
- DVD Library
- Rip DVD to USB


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

The installer does not fail if the file is missing. If it is already
there, the installer leaves it unchanged.


## Uninstall

**Scripts → Uninstall MiSTer DVD Player**

That stops the player/launcher/rip tools, removes the marked
`user-startup` and `[DVD-Player] main=` blocks, and deletes
`/media/fat/MiSTer_DVD_Player.rbf` and `/media/fat/MiSTer_DVD`.

It does **not** delete:

- Stock `/media/fat/MiSTer`
- `/media/fat/DVD/` (ISOs, cache, logs, binaries, user-supplied
  `libdvdcss.so.2`)

To wipe those as well, delete the `DVD` folder on the SD card yourself.


## After install: where files live

| Path | Role |
|---|---|
| `/media/fat/MiSTer` | Stock MiSTer Main (untouched) |
| `/media/fat/MiSTer_DVD` | Core-specific Main for DVD-Player |
| `/media/fat/MiSTer_DVD_Player.rbf` | FPGA core |
| `/media/fat/DVD/dev/` | Launcher, player, rip tools |
| `/media/fat/DVD/lib/` | libdvdread, libdvdnav; optional libdvdcss |
| `/media/fat/DVD/isos/` | SD ISO library |
| `/media/fat/games/DVD-Player` | MiSTer games/home dir for this core |
| `/media/fat/MiSTer.ini` | `[DVD-Player] main=MiSTer_DVD` |
