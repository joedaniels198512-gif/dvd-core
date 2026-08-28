# Credits — MiSTer DVD Player

## This project

- **Mojojojo198512** — MiSTer DVD Player (launcher branding and this
  public beta)

## Architecture guidance (not copied source)

- **Anime0t4ku** — Physical Disc / MiSTer core-specific `main=` pattern
  (`[CD-*] main=MiSTer_Physical-CD` and related Main_MiSTer Physical Disc
  work). That project informed how DVD-Player uses
  `[DVD-Player] main=MiSTer_DVD` beside stock `/media/fat/MiSTer`. DVD
  player, launcher, FPGA, and ripping code in this repository are not a
  copy of that source tree.

## MiSTer

- **Main_MiSTer** contributors (Dennis van Weeren, Jakub Bednarski,
  Till Harbaum, Alexey Melnikov, and others) — stock Main at commit
  `0a8fb44`, from which `MiSTer_DVD` is derived
- **MiSTer FPGA framework** (`fpga/sys/`) — Alexey Melnikov and
  contributors (GPL-2-or-later)

## Playback stack

- **FFmpeg** developers — FFmpeg 6.0.1 (LGPL-2.1+), statically linked
  into the player
- **VideoLAN** and libdvdnav/libdvdread authors — navigation and disc
  access (Debian armhf binaries in this package)
- **VideoLAN libdvdcss** — optional, **not bundled**; official project
  at https://www.videolan.org/developers/libdvdcss.html

## Ripping stack (shipped)

- **dvdbackup** — Olaf Beck, Benjamin Drung, and Debian maintainers
  (Debian `dvdbackup` 0.4.2-4.1)
- **cdrkit / genisoimage** — Eric Youngdale, J. Schilling, Debian
  cdrkit maintainers, and others (Debian `genisoimage` 9:1.1.11-3.2)
- **file / libmagic** — Ian F. Darwin, Christos Zoulas, and others
  (Debian `file` 5.39)
- **zlib** — Jean-loup Gailly and Mark Adler (Debian `zlib1g`)
- **bzip2** — Julian Seward (Debian `libbz2-1.0`)

## Debian

Redistributed ARM libraries and ripping tools are **Debian bullseye /
matching armhf binaries**. Corresponding source is the Debian source
packages, not a VideoLAN tarball alone. See [SOURCE_INFO.md](SOURCE_INFO.md).

## Other

- **Altera/Intel Quartus** MegaWizard PLL in `fpga/rtl/pll.v` (vendor
  generated; compiled into the RBF)
- Independent JPEG Group — FFmpeg documents certain DCT files derived
  from libjpeg; see `LICENSES/FFmpeg-LICENSE.md`

Licence texts: [LICENSES/](LICENSES/), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

No additional hardware testers are named here beyond what is already
recorded in project files.
