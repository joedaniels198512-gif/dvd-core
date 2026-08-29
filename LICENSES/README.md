# LICENSES — texts for components actually distributed

Public **v0.2.0-beta.1** ships playback only. It does **not** include a
libdvdcss licence: `libdvdcss.so.2` is not distributed. The historical
0.1 zip also shipped a Rip DVD stack; that helper is not in v0.2.

| File | Covers |
|---|---|
| `COPYING.GPLv2` | GPL-2 text (player/launcher/rip combination with libdvdnav/libdvdread; FPGA GPL-2-or-later; genisoimage/cdrkit) |
| `FPGA-COPYING.GPLv2` | Same GPL-2 text as shipped in `fpga/LICENSE` |
| `COPYING.GPLv3` | FSF GPL-3 text (dvdbackup GPL-3+; also available for Main) |
| `Main_MiSTer-COPYING.GPLv3` | Exact `LICENSE` from Main_MiSTer commit `0a8fb44` |
| `FFmpeg-COPYING.LGPLv2.1` | LGPL-2.1 for the FFmpeg 6.0.1 code statically linked into the player |
| `FFmpeg-LICENSE.md` | FFmpeg 6.0.1 licence map (including IJG documentation duty) |
| `libdvdread.copyright` | Debian copyright file from `libdvdread8` 6.1.1-2 armhf |
| `libdvdnav.copyright` | Debian copyright file from `libdvdnav4` 6.1.0-1+b1 armhf |
| `dvdbackup.copyright` | Debian copyright file from `dvdbackup` 0.4.2-4.1 armhf |
| `genisoimage.copyright` | Debian copyright file from `genisoimage` 9:1.1.11-3.2 armhf |
| `libmagic.copyright` | Debian copyright file from `libmagic1` 1:5.39-3+deb11u1 armhf |
| `zlib.copyright` | Debian copyright file from `zlib1g` 1:1.2.11.dfsg-2+deb11u2 armhf |
| `bzip2.copyright` | Debian copyright file from `libbz2-1.0` 1.0.8-4 armhf |

See `LICENSING.md`, `SOURCE_INFO.md`, and `THIRD_PARTY_NOTICES.md` at the
repository root.
