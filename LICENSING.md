# Licensing map — MiSTer DVD Player

This is not a single-licence repository. Licences follow the work, not a
blanket root statement. Public beta 0.1 ships the binaries in
`COMPLIANCE_PUBLIC_BETA_0.1.md` and `SOURCE_INFO.md`. It does **not**
ship a `libdvdcss` binary. **Rip DVD to USB** is included.

Frozen RC commit: `bf9197036a526e409ca22a869a6ba03e2fa65849`  
Playback behaviour, launcher UI, and launcher font are not changed by
this licensing work.


## Original ARM player / launcher / rip helper (GPL-2.0-or-later)

These files are original to this project and are combined with GPL-2+
libdvdnav and/or libdvdread. They are licensed **GPL-2.0-or-later**.

- `player/tools/dvd_av_threaded_test.c`
- `player/tools/dvd_launcher.c`
- `player/tools/dvd_library.c`
- `player/tools/dvd_library.h`
- `player/tools/dvd_rip_iso.c`
- `player/tools/build_dvd_av_threaded_test.sh`
- `player/tools/build_dvd_launcher.sh`
- `player/tools/build_dvd_rip_iso.sh`
- `player/build_mac.sh`
- `player/tools/dvd_autostart_daemon.sh` (source in-tree; not started by public installs)

SPDX: `GPL-2.0-or-later`.

The launcher embeds an 8×8 glyph table (`font8x8`). Provenance of that
table is **unresolved**; see `COMPLIANCE_PUBLIC_BETA_0.1.md`. The table is
not replaced or relicensed.


## Main_MiSTer-derived MiSTer_DVD (GPL-3)

`MiSTer_DVD` is Main_MiSTer at `0a8fb44ccec6d69c8b7f158abd5fe8065ab2bf4f`
plus this project's overlay. Upstream `LICENSE` and `menu.cpp` state
**GNU GPL version 3**. Overlay files therefore use **GPL-3.0-or-later**
so they can be combined with that tree. Do not relicense Main as GPL-2.

- `main-dvd/dvd_main.cpp`
- `main-dvd/dvd_main.h`
- `main-dvd/apply_dvd_hooks.py`
- `main-dvd/build_mister_dvd.sh`

Upstream sources (gitignored snapshot `main-dvd/.src/`) keep their own
headers. Do not rewrite them.

The `MiSTer_DVD` binary is a modified GPL-3 program. Corresponding source
is the recorded Main snapshot plus the overlay files above.


## FPGA / MiSTer HDL

- `fpga/sys/` — MiSTer framework. Headers are **GPL-2-or-later** (e.g.
  `sys_top.v`). **Do not modify** those files for licensing cosmetics.
- `fpga/DVD.sv` and original DVD RTL under `fpga/rtl/` that we authored
  (`fb_line_reader.v`, `yuv601_rgb.v`, `yuv_plane_addr.v`,
  `yuv_chroma_row.v`, `mycore.v`) — **GPL-2.0-or-later**, matching the
  existing `DVD.sv` GPL-2-or-later grant. `fpga/LICENSE` is the GPL-2 text.
- `fpga/rtl/pll.v` — Altera/Intel MegaWizard-generated PLL. **Not** our
  GPL grant; keep the vendor generation notice.
- Other template leftovers (`lfsr.v`, `cos.sv`) retain whatever licence
  they arrived with from the MiSTer template; they were not re-licensed
  here.


## Third-party libraries (distributed)

| Component | Licence | Notes |
|---|---|---|
| FFmpeg 6.0.1 (static in player) | LGPL-2.1+ | No `--enable-gpl` / `--enable-nonfree`. See `LICENSES/FFmpeg-LICENSE.md`. |
| libdvdread 6.1.1 (`libdvdread.so.8.0.0`) | GPL-2+ (Debian `Files: *`) | Debian binary `libdvdread8` 6.1.1-2 armhf. |
| libdvdnav 6.1.0 (`libdvdnav.so.4.3.0`) | GPL-2+ | Debian binary `libdvdnav4` 6.1.0-1+b1 armhf. |
| dvdbackup 0.4.2 | GPL-3+ | Debian binary `dvdbackup` 0.4.2-4.1 armhf. Combined work with libdvdread is GPL-3+. |
| genisoimage (cdrkit 1.1.11) | GPL-2 | Debian binary `genisoimage` 9:1.1.11-3.2 armhf. |
| libmagic 5.39 + `magic.mgc` | BSD-2-Clause-alike | Required by genisoimage. Debian `file` 1:5.39-3+deb11u1. |
| zlib 1.2.11 | Zlib | Required by genisoimage. Debian `zlib1g` 1:1.2.11.dfsg-2+deb11u2. |
| bzip2 1.0.8 (`libbz2`) | bzip2 BSD-variant (Debian `debian/*` is GPL-2) | Required by genisoimage. Debian `libbz2-1.0` 1.0.8-4. |

Corresponding source for Debian armhf binaries is the **Debian source
package**, not an upstream tarball alone. See `SOURCE_INFO.md` and
`release/corresponding-source/`.

dvdbackup is GPL-3+. Shipping it next to GPL-2+ libdvdread is compatible
(GPL-3+ can combine with GPL-2+). The overall public zip therefore
contains both GPL-2-or-later and GPL-3+ components; each file keeps its
own terms.


## Intentionally not distributed (public beta 0.1)

- `libdvdcss` (any soname). Optional user-supplied runtime at
  `/media/fat/DVD/lib/libdvdcss.so.2`. The player and ripping path still
  `dlopen` it via libdvdread if present. CSS support is not removed.
  The installer must not fail solely because it is absent, and must
  preserve an existing copy.


## Experimental / non-package trees

Other `player/tools/*.c` benches and deploy scripts are development aids.
They are not in the public binary package. If distributed later, treat
original files that link libdvdnav/libdvdread as GPL-2.0-or-later.


## What a public zip must ship

Licence texts in `LICENSES/`, this map, `THIRD_PARTY_NOTICES.md`, and
`SOURCE_INFO.md`, plus the Debian source triple for each shipped Debian
binary, the FFmpeg 6.0.1 tarball, and our player/launcher/rip/Main overlay
sources.

User-facing terms: `README.md`, `INSTALL.md`, `LEGAL.md`.
