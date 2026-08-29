# Install MiSTer DVD Player (v0.2.0 beta 1)

No installer script is required. Extract the zip onto the SD card, then
**add** one `MiSTer.ini` section.


## Manual install (GitHub zip)

### 1. Extract to the SD root

Copy **`MiSTer_DVD_Player_0.2.0-beta.1.zip`** to the MiSTer SD card and
extract its contents at the **root** of the FAT partition (`/media/fat`
when the system is running). You should have:

```
/media/fat/DVD_Player.rbf
/media/fat/MiSTer_DVD
/media/fat/DVD/bin/          dvd_player, dvd_av_threaded_test
/media/fat/DVD/lib/          libdvdread, libdvdnav
/media/fat/DVD/logs/
/media/fat/DVD/config/
/media/fat/games/DVD-Player/
```

Licence and corresponding-source files from the zip may sit beside those
paths; they are not required to run the player.

**Do not replace** stock `/media/fat/MiSTer` with `MiSTer_DVD`. Both
files must remain on the SD root. `MiSTer_DVD` is used only for this
core.

If you already have a user-supplied `libdvdcss.so.2` under `DVD/lib`,
leave that file in place. This zip does not include it.


### 2. Edit MiSTer.ini (add, do not replace)

Open `/media/fat/MiSTer.ini` in a text editor.

**Add** this section. Do **not** overwrite or replace your existing
`MiSTer.ini`.

```
[DVD-Player]
main=MiSTer_DVD
```

Save. Reboot or return to the MiSTer menu so the new core setting is
picked up.


### 3. Launch DVD Player

From the MiSTer main menu, start **DVD Player** (`DVD_Player.rbf`,
core name `DVD-Player`).

A physical DVD already in the drive autoplays. Use OSD **Play ISO...**
for ISO files.


## ISO folders

- SD (created by the package / at first run):
  `/media/fat/games/DVD-Player/`
- USB (create this yourself if you want USB ISOs):
  `/games/DVD-Player/` on the USB drive  
  Example: `/media/usb0/games/DVD-Player/Movie.iso`

MiSTer uses the USB core folder when it exists, otherwise SD. Empty USB
core folders are not created automatically. Existing ISOs are never
moved or copied.


## CSS-encrypted discs

This package does **not** include libdvdcss (`libdvdcss.so.2` or any
other libdvdcss binary). Unencrypted discs and ISOs do not need it.
Encrypted commercial DVDs require you to obtain and install libdvdcss
separately. If you do, the file must be named exactly `libdvdcss.so.2`
at:

`/media/fat/DVD/lib/libdvdcss.so.2`

See [LEGAL.md](LEGAL.md).


## Logs

- `/media/fat/DVD/logs/player.log`
- `/media/fat/DVD/logs/dvd_player.log`


## Uninstall (manual)

1. Delete `/media/fat/DVD_Player.rbf` and `/media/fat/MiSTer_DVD`
2. Optionally remove `/media/fat/DVD/` (keep `DVD/lib/libdvdcss.so.2`
   if you added it)
3. Remove or comment `[DVD-Player] main=MiSTer_DVD` in `MiSTer.ini`
4. Your ISOs in `games/DVD-Player/` are left alone unless you delete
   them
