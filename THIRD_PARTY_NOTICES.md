# Third-party notices — public beta 0.1

Exact versions. No CSS binary is distributed. Rip DVD to USB **is**
distributed.


## FFmpeg 6.0.1

Copyright the FFmpeg developers.

This package incorporates FFmpeg 6.0.1 as a **static** library inside
`dvd_av_threaded_test`, built from https://ffmpeg.org/releases/ffmpeg-6.0.1.tar.xz
with the configure flags in `player/build_mac.sh`.

Licence: **LGPL v2.1 or later**. `CONFIG_GPL=0`, `CONFIG_NONFREE=0`.
Texts: `LICENSES/FFmpeg-COPYING.LGPLv2.1`, `LICENSES/FFmpeg-LICENSE.md`.
Corresponding source: `SOURCES/ffmpeg-6.0.1.tar.xz`.

FFmpeg `LICENSE.md` states that `libavcodec/jfdctfst.c`,
`libavcodec/jfdctint_template.c`, and `libavcodec/jrevdct.c` are taken from
libjpeg. If those files are compiled into the linked `libavcodec.a`,
documentation accompanying executables must credit the Independent JPEG
Group and note any changes to those three files. This project does not
modify those files. No IJG banner string was found in the RC player
binary; the documentation duty from FFmpeg's LICENSE.md is recorded here
anyway.


## libdvdread 6.1.1 (Debian libdvdread8 6.1.1-2 armhf)

Copyright holders as listed in `LICENSES/libdvdread.copyright`
(VideoLAN / xine / listed authors). Licence: **GPL-2+** for `Files: *`.

Binary SHA-256 `222de9bd1f40c01514f8dd88fa30bb669aaae4c09959dbf26dc15061cff534c8`.
Corresponding source: Debian `libdvdread` 6.1.1-2 (see `SOURCE_INFO.md`).
Debian patches: `0001-libdvdcss.patch`, `0002-descriptor.patch`.

The library may `dlopen` `libdvdcss.so.2`. That library is **not** part of
this package.


## libdvdnav 6.1.0 (Debian libdvdnav4 6.1.0-1+b1 armhf)

Copyright holders as listed in `LICENSES/libdvdnav.copyright`.
Licence: **GPL-2+**.

Binary SHA-256 `2e113c2e65911713778537947644940278928d2eff3f04390fcafd4aef15980f`.
Corresponding source: Debian `libdvdnav` 6.1.0-1 (binNMU `+b1` has no
source changes). No `debian/patches` in that source version.


## dvdbackup 0.4.2 (Debian dvdbackup 0.4.2-4.1 armhf)

Copyright Olaf Beck, Benjamin Drung, and Debian maintainers as listed in
`LICENSES/dvdbackup.copyright`. Licence: **GPL-3+**.

Binary SHA-256 `fbfa1a1f4750a88a59989cd60bc40820fbd61e3b09f29f0738849cb78e6da320`.
Corresponding source: Debian `dvdbackup` 0.4.2-4.1 (see `SOURCE_INFO.md`).
Debian patches include `libdvdread6.1.0.patch`.

Used only by **Rip DVD to USB** (`dvd_rip_iso` → `dvdbackup -M`).
May `dlopen` CSS via libdvdread; CSS is not shipped.


## genisoimage / cdrkit 1.1.11 (Debian genisoimage 9:1.1.11-3.2 armhf)

Copyright holders as listed in `LICENSES/genisoimage.copyright`
(Eric Youngdale, J. Schilling, Debian cdrkit maintainers, and others).
Licence: **GPL-2**.

Binary SHA-256 `a114528e04c5262387b358baebc252e31b2be8de4df4e87ca130ee35ac3d7d8e`.
Corresponding source: Debian `cdrkit` 1.1.11-3.2.
Only the `genisoimage` binary is redistributed, not the rest of the
cdrkit tool set.

Used only by **Rip DVD to USB** (`dvd_rip_iso` → `genisoimage -dvd-video`).


## libmagic 5.39 + magic.mgc (Debian file 1:5.39-3+deb11u1 armhf)

Copyright Ian F. Darwin, Christos Zoulas, and others as listed in
`LICENSES/libmagic.copyright`. Licence: **BSD-2-Clause-alike** (plus
related BSD/MIT notices in that file).

`libmagic.so.1.0.0` SHA-256
`a9693bd66fe1782b5d02043e243dd56e73897d3d4c3a4b17007c4313b9ca1e60`.
`magic.mgc` SHA-256
`5f7ada0953e39fa859e15eff4ac3f3fc2064cac23769019d6aac7b53cb2c9c54`.
Required by genisoimage. Corresponding source: Debian `file` 5.39-3+deb11u1.


## zlib 1.2.11 (Debian zlib1g 1:1.2.11.dfsg-2+deb11u2 armhf)

Copyright Jean-loup Gailly and Mark Adler. Licence: **Zlib**
(`LICENSES/zlib.copyright`).

`libz.so.1.2.11` SHA-256
`e8f5ff20a6dd792bef144d2f3280c62189279f17c0c41db6a32cf7c8534c681a`.
Required by genisoimage. Corresponding source: Debian `zlib`
1.2.11.dfsg-2+deb11u2 (dfsg orig, not the vanilla zlib.org tarball).


## bzip2 1.0.8 (Debian libbz2-1.0 1.0.8-4 armhf)

Copyright Julian R Seward. Licence: **bzip2 BSD-variant**
(`LICENSES/bzip2.copyright`). Debian packaging files are GPL-2.

`libbz2.so.1.0.4` SHA-256
`6b815fd378d384641e01950b222f86f2dcb163f0ea80afffced4ee5a564d0120`.
Required by genisoimage. Corresponding source: Debian `bzip2` 1.0.8-4.


## Main_MiSTer (inside MiSTer_DVD)

Copyright Dennis van Weeren, Jakub Bednarski, Till Harbaum, Alexey
Melnikov, and other Main_MiSTer contributors as in the upstream tree.

Licence: **GPL-3** (`LICENSES/Main_MiSTer-COPYING.GPLv3`), commit
`0a8fb44ccec6d69c8b7f158abd5fe8065ab2bf4f`.

That binary also contains Main's bundled third-party code (not shipped as
separate files by us), including at least:

- libchdr / CHD (Aaron Giles; BSD-style notice in `chd.h`)
- zstd, lzma/7zip SDK, miniz, libco, sxmlc — licences as in the Main
  snapshot

Corresponding source: that Main commit plus `main-dvd/dvd_main.cpp` and
`main-dvd/apply_dvd_hooks.py`.


## MiSTer FPGA framework (`fpga/sys/`)

Copyright Alexey Melnikov and MiSTer contributors.
Licence: **GPL-2 or later** (file headers such as `sys_top.v`).
Text: `LICENSES/FPGA-COPYING.GPLv2` / `fpga/LICENSE`.


## Altera/Intel PLL (`fpga/rtl/pll.v`)

Generated by Quartus 17.0 MegaWizard. Copyright notice in that file is
Altera Corporation's. Not redistributed as a standalone library; compiled
into the RBF.


## SS1 system libraries (not in our zip)

`MiSTer_DVD` dynamically needs libraries already on the SuperStation One
image (`libc`, `libstdc++`, `libfreetype`, `libpng`, `libz`, `libbz2`,
`libImlib2`, `libbluetooth`, …). This package does not copy those files
for Main. The ripping path **does** ship its own `libz` / `libbz2` /
`libmagic` under `DVD/dev/rip-lib` (and a copy under `DVD/lib`) because
genisoimage is a Debian armhf binary that `NEEDED`s those sonames
regardless of what Main uses.


## Names

“FFmpeg”, “VideoLAN”, “MiSTer”, and “DVD” are used only to identify
software and the disc format. No affiliation or endorsement is claimed.
