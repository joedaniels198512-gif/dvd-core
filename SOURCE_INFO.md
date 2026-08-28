# Corresponding source and provenance — public beta 0.1

Frozen RC: `bf9197036a526e409ca22a869a6ba03e2fa65849`  
Branch: `exp/custom-main-dvd-player`

This file records **exact** versions and download references for everything
the public package redistributes. It does **not** claim that an upstream
tarball alone is corresponding source for a Debian-patched binary.

`libdvdcss` is **not** redistributed.


## Our binaries

| File | SHA-256 | Source |
|---|---|---|
| RBF (`DVD_FPGA_YUV420_DVDPlayer_Main.rbf` / on-device ResetFix Test bytes) | `3d841dc03aacfd58e21de8a7f0721d45bbfbf0c5d0b48bed760fa270c74c24f5` | FPGA commit `1e716f156a53d60856e394ead43ad10de5877674`; GHA run 33175603689 |
| `MiSTer_DVD` | `846eb22318bc6fd59c72e766daba6f9b40179b5bd231e358b8a9bfb89b8ade84` | Main_MiSTer `0a8fb44ccec6d69c8b7f158abd5fe8065ab2bf4f` + overlay commits through `4a3a6fc3f1755b6972ebc5adeefc9fe7679e3355` |
| `dvd_av_threaded_test` | `89ed3327df140d98fc5e2c7edcf6312d798f8ed50ba82aa70abe3404be0df09c` | `player/tools/dvd_av_threaded_test.c` at `89e9a793742d6d6f14b19942b1fef1e32416fb97` (plus later SPDX-only line) |
| `dvd_launcher` | `f4fea091a7fdb53200cb5ab9a65c21d051f127ea6fd6e0e6aa0c01f0ce39e58f` | `player/tools/dvd_launcher.c` at `08c7f75622cc8d579af25fd6e944c74ac90cf9c3` |
| `dvd_rip_iso` | `42aec76ee04e6eb5b70c962ab764a6eeed897f2876cfc45cb0a39b39df51cecf` | `player/tools/dvd_rip_iso.c` (GPL-2.0-or-later); built by `build_dvd_rip_iso.sh` |

Player/launcher **source** after SPDX comments is still the RC behaviour;
those comment-only edits are not a binary rebuild. Launcher font bytes are
unchanged (`font8x8 provenance unresolved`; see
`COMPLIANCE_PUBLIC_BETA_0.1.md`).


## FFmpeg 6.0.1 (static, LGPL-2.1+)

- Upstream: https://ffmpeg.org/releases/ffmpeg-6.0.1.tar.xz
- Tarball SHA-256: `9b16b8731d78e596b4be0d720428ca42df642bb2d78342881ff7f5bc29fc9623`
- Configure: `player/build_mac.sh` (`--disable-everything`; MPEG-PS; mpeg1/2;
  ac3; eac3; mp1/2/3; dca; pcm_dvd; **no** `--enable-gpl`; **no**
  `--enable-nonfree`)
- Linked into `dvd_av_threaded_test` only: libavformat, libavcodec,
  libswscale, libswresample, libavutil
- Build evidence: `player/.build/ffmpeg-6.0.1/config.h` has
  `CONFIG_GPL 0`, `CONFIG_NONFREE 0`
- Corresponding source in the zip: `SOURCES/ffmpeg-6.0.1.tar.xz`


## libdvdread — Debian corresponding source

Do **not** treat `libdvdread-6.1.1.tar.bz2` from VideoLAN as the sole
corresponding source.

| Field | Value |
|---|---|
| Binary package | `libdvdread8` |
| Binary version | `6.1.1-2` |
| Architecture | `armhf` |
| Debian `.deb` used | `libdvdread8_6.1.1-2_armhf.deb` |
| `.deb` SHA-256 | `acb36d04507f8e0bb03c0662c3bb8855e6c99c6f180c4ee7bd7e077e381c7ce1` |
| Shipped `.so` | `libdvdread.so.8.0.0` (also installed as `libdvdread.so.8`) |
| `.so` SHA-256 | `222de9bd1f40c01514f8dd88fa30bb669aaae4c09959dbf26dc15061cff534c8` |
| Upstream version | 6.1.1 |
| Source package | `libdvdread` |
| Source version | `6.1.1-2` |
| Suite | Debian bullseye / oldoldstable (binary `6.1.1-2`) |
| Homepage | https://www.videolan.org/developers/libdvdnav.html |
| Suggests | `libdvdcss2` (not installed by our package) |
| Licence text | `LICENSES/libdvdread.copyright` |
| Corresponding source required | Yes (GPL-2+) |

Debian source triple (files in `release/corresponding-source/`):

| File | SHA-256 |
|---|---|
| `libdvdread_6.1.1-2.dsc` | `ba70e43e8f3e7154d8d3b808181f59230716aab16189039ca3907b624eb82fec` |
| `libdvdread_6.1.1.orig.tar.bz2` | `3e357309a17c5be3731385b9eabda6b7e3fa010f46022a06f104553bf8e21796` |
| `libdvdread_6.1.1-2.debian.tar.xz` | `162340bad7c8180a6e027155f2a8597903136eaf4da54bb8e6a5a9d017c530c1` |

URLs:

- https://deb.debian.org/debian/pool/main/libd/libdvdread/libdvdread_6.1.1-2.dsc
- https://deb.debian.org/debian/pool/main/libd/libdvdread/libdvdread_6.1.1.orig.tar.bz2
- https://deb.debian.org/debian/pool/main/libd/libdvdread/libdvdread_6.1.1-2.debian.tar.xz
- https://packages.debian.org/source/oldoldstable/libdvdread

Debian patches applied to upstream 6.1.1 (`debian/patches/series`):

1. `0001-libdvdcss.patch` — Daniel Baumann; expands the “Encrypted DVD support
   unavailable” log to point at Debian `README.css`.
2. `0002-descriptor.patch` — Mario Holbe; UDF File System Descriptor lookup
   (Closes: #663512).

Copies: `release/corresponding-source/libdvdread-6.1.1-2-debian-patches/`.


## libdvdnav — Debian corresponding source

| Field | Value |
|---|---|
| Binary package | `libdvdnav4` |
| Binary version | `6.1.0-1+b1` |
| Architecture | `armhf` |
| Debian `.deb` used | `libdvdnav4_6.1.0-1+b1_armhf.deb` |
| `.deb` SHA-256 | `724c876c647bd244accd36ae258e84b1ce4dbfc2d7499ca6cc9ddc9800b21c7c` |
| Shipped `.so` | `libdvdnav.so.4.3.0` (also installed as `libdvdnav.so.4`) |
| `.so` SHA-256 | `2e113c2e65911713778537947644940278928d2eff3f04390fcafd4aef15980f` |
| Upstream version | 6.1.0 |
| Source package | `libdvdnav` |
| Source version | `6.1.0-1` (the `+b1` is a **binNMU**: binary-only rebuild against
  libdvdread8; changelog.Debian.armhf: “no source changes”) |
| Depends | `libdvdread8 (>= 6.1.0)` |
| Licence text | `LICENSES/libdvdnav.copyright` |
| Corresponding source required | Yes (GPL-2+) |

Debian source triple:

| File | SHA-256 |
|---|---|
| `libdvdnav_6.1.0-1.dsc` | `d0984eaaf66a6415f2b88cd6c94f710c2de999dd83345ab5b78b076a736dc6df` |
| `libdvdnav_6.1.0.orig.tar.bz2` | `f697b15ea9f75e9f36bdf6ec3726308169f154e2b1e99865d0bbe823720cee5b` |
| `libdvdnav_6.1.0-1.debian.tar.xz` | `908043ff6493aea31c5352808aef4168ebe942187306cc8ce023e7fb322f206e` |

URLs:

- https://deb.debian.org/debian/pool/main/libd/libdvdnav/libdvdnav_6.1.0-1.dsc
- https://deb.debian.org/debian/pool/main/libd/libdvdnav/libdvdnav_6.1.0.orig.tar.bz2
- https://deb.debian.org/debian/pool/main/libd/libdvdnav/libdvdnav_6.1.0-1.debian.tar.xz

`debian/patches/`: **none** in `6.1.0-1`. Corresponding source is still the
Debian orig + debian packaging (rules, changelog, copyright), not “orig
tarball only” as a policy statement: the binary we ship is the Debian
armhf build `6.1.0-1+b1`.


## dvdbackup — Debian corresponding source

Do **not** treat the SourceForge `dvdbackup-0.4.2.tar.gz` as the sole
corresponding source. The shipped binary is Debian `0.4.2-4.1`, which
applies `libdvdread6.1.0.patch`.

| Field | Value |
|---|---|
| Binary package | `dvdbackup` |
| Binary version | `0.4.2-4.1` |
| Architecture | `armhf` |
| Debian `.deb` used | `dvdbackup_0.4.2-4.1_armhf.deb` |
| `.deb` SHA-256 | `e8e25dac85384d0d2bd1b6c20c6b85732a3f0365750830dbb5f138d4c3d1c464` |
| Shipped binary | `DVD/dev/dvdbackup` |
| Binary SHA-256 | `fbfa1a1f4750a88a59989cd60bc40820fbd61e3b09f29f0738849cb78e6da320` |
| ELF NEEDED | `libdvdread.so.8`, `libc.so.6`, `ld-linux-armhf.so.3` |
| Upstream version | 0.4.2 |
| Source package | `dvdbackup` |
| Source version | `0.4.2-4.1` |
| Homepage | http://dvdbackup.sourceforge.net |
| Licence | GPL-3+ |
| Licence text | `LICENSES/dvdbackup.copyright` + `LICENSES/COPYING.GPLv3` |
| Corresponding source required | Yes (GPL-3+) |
| Suggests | `libdvdcss2` (not installed by our package) |

Debian source triple:

| File | SHA-256 |
|---|---|
| `dvdbackup_0.4.2-4.1.dsc` | `97630e6fbe7c81a3555fc313d16bdb8b59ad59d6c5f303d1ff87fe067392f1cf` |
| `dvdbackup_0.4.2.orig.tar.xz` | `ef8c56fbb82b15b7eef00d2d3118c8253f9770009ed7bb2a5d4849acf88183e6` |
| `dvdbackup_0.4.2-4.1.debian.tar.xz` | `9a7563696291c309ccdb07cbadc0b1457edc6ddb26f670dddc630c094ddd53eb` |

URLs:

- https://deb.debian.org/debian/pool/main/d/dvdbackup/dvdbackup_0.4.2-4.1.dsc
- https://deb.debian.org/debian/pool/main/d/dvdbackup/dvdbackup_0.4.2.orig.tar.xz
- https://deb.debian.org/debian/pool/main/d/dvdbackup/dvdbackup_0.4.2-4.1.debian.tar.xz

Debian patches (`debian/patches/series`):

1. `ignore-automake-warnings.patch`
2. `remove-path_max-limitation.patch`
3. `libdvdread6.1.0.patch` — adapt to libdvdread 6.1.0 API (Closes: #955652)

Copies: `release/corresponding-source/dvdbackup-0.4.2-4.1-debian-patches/`.


## genisoimage / cdrkit — Debian corresponding source

Only `usr/bin/genisoimage` from that package is shipped. Extra cdrkit
tools in the same `.deb` (`isoinfo`, `mkzftree`, `geteltorito`, …) are
**not** on the public ripping path and are not copied.

| Field | Value |
|---|---|
| Binary package | `genisoimage` |
| Binary version | `9:1.1.11-3.2` |
| Architecture | `armhf` |
| Debian `.deb` used | `genisoimage_1.1.11-3.2_armhf.deb` (local fetch name `genisoimage_armhf.deb`) |
| `.deb` SHA-256 | `a0a0d832c2d4e927da448e7a1b931fd3854cfeaac100d0c4d1603314fa490dde` |
| Shipped binary | `DVD/dev/genisoimage` |
| Binary SHA-256 | `a114528e04c5262387b358baebc252e31b2be8de4df4e87ca130ee35ac3d7d8e` |
| ELF NEEDED | `libmagic.so.1`, `libz.so.1`, `libbz2.so.1.0`, `libc.so.6`, `libpthread.so.0` |
| Upstream | cdrkit 1.1.11 |
| Source package | `cdrkit` |
| Source version | `1.1.11-3.2` |
| Licence | GPL-2 (Debian copyright for mkisofs/genisoimage) |
| Licence text | `LICENSES/genisoimage.copyright` + `LICENSES/COPYING.GPLv2` |
| Corresponding source required | Yes (GPL-2) |

Debian source triple:

| File | SHA-256 |
|---|---|
| `cdrkit_1.1.11-3.2.dsc` | `f193e74f8af47e5b952a36c2e314a9e9bb18eb16db070a2c3ed58d1abb05a16a` |
| `cdrkit_1.1.11.orig.tar.gz` | `d1c030756ecc182defee9fe885638c1785d35a2c2a297b4604c0e0dcc78e47da` |
| `cdrkit_1.1.11-3.2.debian.tar.xz` | `d4e41340e8e9bfd8df167eef13756013fa21bf5fbe438b30c288add6abba24d9` |

URLs:

- https://deb.debian.org/debian/pool/main/c/cdrkit/cdrkit_1.1.11-3.2.dsc
- https://deb.debian.org/debian/pool/main/c/cdrkit/cdrkit_1.1.11.orig.tar.gz
- https://deb.debian.org/debian/pool/main/c/cdrkit/cdrkit_1.1.11-3.2.debian.tar.xz


## libmagic / magic.mgc — Debian corresponding source (`file`)

Required by genisoimage (`NEEDED libmagic.so.1` + compiled magic database).

| Field | Value |
|---|---|
| Binary packages | `libmagic1`, `libmagic-mgc` |
| Binary version | `1:5.39-3+deb11u1` |
| Architecture | `armhf` |
| `.deb` SHA-256 (`libmagic1`) | `f9a7d952b0d9f49e78949721944e5309cd84eecb9fb04810bd250d7ab9da2a89` |
| `.deb` SHA-256 (`libmagic-mgc`) | `c394077746381455aec1850b50d4c696e40f718d55fdb136f4c218d57d604069` |
| Shipped `.so` | `libmagic.so.1.0.0` (also as `libmagic.so.1`) |
| `.so` SHA-256 | `a9693bd66fe1782b5d02043e243dd56e73897d3d4c3a4b17007c4313b9ca1e60` |
| Shipped `magic.mgc` | `DVD/dev/magic.mgc` (Debian payload `usr/lib/file/magic.mgc`; other paths in the `.deb` are symlinks) |
| `magic.mgc` SHA-256 | `5f7ada0953e39fa859e15eff4ac3f3fc2064cac23769019d6aac7b53cb2c9c54` |
| Source package | `file` |
| Source version | `5.39-3+deb11u1` |
| Homepage | https://www.darwinsys.com/file/ |
| Licence | BSD-2-Clause-alike (and related BSD/MIT notices in the Debian copyright) |
| Licence text | `LICENSES/libmagic.copyright` |
| Corresponding source required | Not by the BSD-2-Clause-alike terms; Debian triple is still shipped so the exact armhf build can be reproduced. |

Debian source triple:

| File | SHA-256 |
|---|---|
| `file_5.39-3+deb11u1.dsc` | `b08cfd706099600aa634d3cdcc2a7461908902414ae8cbfd1caddf557983e4e8` |
| `file_5.39.orig.tar.gz` | `f05d286a76d9556243d0cb05814929c2ecf3a5ba07963f8f70bfaaa70517fad1` |
| `file_5.39-3+deb11u1.debian.tar.xz` | `c4ef624328d06f6128d808fd0edc14b1d856d6fefe0f29dfcae0ce30b42de0a3` |

URLs:

- https://deb.debian.org/debian/pool/main/f/file/file_5.39-3+deb11u1.dsc
- https://deb.debian.org/debian/pool/main/f/file/file_5.39.orig.tar.gz
- https://deb.debian.org/debian/pool/main/f/file/file_5.39-3+deb11u1.debian.tar.xz


## zlib — Debian corresponding source

Required by genisoimage (`NEEDED libz.so.1`).

| Field | Value |
|---|---|
| Binary package | `zlib1g` |
| Binary version | `1:1.2.11.dfsg-2+deb11u2` |
| Architecture | `armhf` |
| `.deb` SHA-256 | `6c30a8d2525b301765c320c2d49dc6c361e1d3cd2ca9225492c2a083e0b74ce4` |
| Shipped `.so` | `libz.so.1.2.11` (also as `libz.so.1`) |
| `.so` SHA-256 | `e8f5ff20a6dd792bef144d2f3280c62189279f17c0c41db6a32cf7c8534c681a` |
| Source package | `zlib` |
| Source version | `1.2.11.dfsg-2+deb11u2` |
| Homepage | http://zlib.net/ |
| Licence | Zlib |
| Licence text | `LICENSES/zlib.copyright` |
| Corresponding source required | Attribution / notice (Zlib). Debian **dfsg** orig (not vanilla `zlib-1.2.11.tar.gz`) is shipped because that is the source of the binary we redistribute. |

Debian source triple:

| File | SHA-256 |
|---|---|
| `zlib_1.2.11.dfsg-2+deb11u2.dsc` | `ec2ee2fc4dfd1f799dfa6a95133ebccbd7531886de38b0a8e3e58b66706a6dc7` |
| `zlib_1.2.11.dfsg.orig.tar.gz` | `80c481411a4fe8463aeb8270149a0e80bb9eaf7da44132b6e16f2b5af01bc899` |
| `zlib_1.2.11.dfsg-2+deb11u2.debian.tar.xz` | `c19794df214f0c2571b19f7dea853c066410232abe9f0ddad77231fabccde0da` |

URLs:

- https://deb.debian.org/debian/pool/main/z/zlib/zlib_1.2.11.dfsg-2+deb11u2.dsc
- https://deb.debian.org/debian/pool/main/z/zlib/zlib_1.2.11.dfsg.orig.tar.gz
- https://deb.debian.org/debian/pool/main/z/zlib/zlib_1.2.11.dfsg-2+deb11u2.debian.tar.xz


## bzip2 / libbz2 — Debian corresponding source

Required by genisoimage (`NEEDED libbz2.so.1.0`).

| Field | Value |
|---|---|
| Binary package | `libbz2-1.0` |
| Binary version | `1.0.8-4` |
| Architecture | `armhf` |
| `.deb` SHA-256 | `e897f1205f7b08edff033ae5403a5498f914e68efd14d288c27a3e1136fed58f` |
| Shipped `.so` | `libbz2.so.1.0.4` (also as `libbz2.so.1.0` and `libbz2.so.1`) |
| `.so` SHA-256 | `6b815fd378d384641e01950b222f86f2dcb163f0ea80afffced4ee5a564d0120` |
| Source package | `bzip2` |
| Source version | `1.0.8-4` |
| Homepage | https://sourceware.org/bzip2/ |
| Licence | bzip2 BSD-variant (`Files: *`); Debian packaging `debian/*` is GPL-2 |
| Licence text | `LICENSES/bzip2.copyright` |
| Corresponding source required | Notice retention (bzip2 licence). Debian triple is shipped because that is the source of the binary we redistribute; `debian/*` is GPL-2. |

Debian source triple (debian tarball is `.bz2`, not `.xz`):

| File | SHA-256 |
|---|---|
| `bzip2_1.0.8-4.dsc` | `662c5e656a87db884fdc070239f5112cba1e616f20ff260de602876f70415c7b` |
| `bzip2_1.0.8.orig.tar.gz` | `ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269` |
| `bzip2_1.0.8-4.debian.tar.bz2` | `3f3b26d83120260c7b2e69a5c89649bb818a79955b960fb34a5fae106f008a5d` |

URLs:

- https://deb.debian.org/debian/pool/main/b/bzip2/bzip2_1.0.8-4.dsc
- https://deb.debian.org/debian/pool/main/b/bzip2/bzip2_1.0.8.orig.tar.gz
- https://deb.debian.org/debian/pool/main/b/bzip2/bzip2_1.0.8-4.debian.tar.bz2


## libdvdcss — not in this package

Not shipped. libdvdread `dlopen`s `libdvdcss.so.2` if the user places a
compatible copy in `/media/fat/DVD/lib/`. Official project:
https://www.videolan.org/developers/libdvdcss.html  
Users must follow applicable local law. This project does not download it.
The installer must not fail solely because it is absent, and must preserve
an existing user-supplied copy.

Without CSS: unencrypted physical DVDs remain usable where supported; ISO
functionality remains available; encrypted physical DVD playback/ripping
may be unavailable.


## Main_MiSTer

- https://github.com/MiSTer-devel/Main_MiSTer
- Commit `0a8fb44ccec6d69c8b7f158abd5fe8065ab2bf4f`
- Overlay: `main-dvd/dvd_main.cpp`, `apply_dvd_hooks.py`


## How to unpack Debian corresponding source

```
dget https://deb.debian.org/debian/pool/main/libd/libdvdread/libdvdread_6.1.1-2.dsc
dpkg-source -x libdvdread_6.1.1-2.dsc

dget https://deb.debian.org/debian/pool/main/libd/libdvdnav/libdvdnav_6.1.0-1.dsc
dpkg-source -x libdvdnav_6.1.0-1.dsc

dget https://deb.debian.org/debian/pool/main/d/dvdbackup/dvdbackup_0.4.2-4.1.dsc
dpkg-source -x dvdbackup_0.4.2-4.1.dsc

dget https://deb.debian.org/debian/pool/main/c/cdrkit/cdrkit_1.1.11-3.2.dsc
dpkg-source -x cdrkit_1.1.11-3.2.dsc

dget https://deb.debian.org/debian/pool/main/f/file/file_5.39-3+deb11u1.dsc
dpkg-source -x file_5.39-3+deb11u1.dsc

dget https://deb.debian.org/debian/pool/main/z/zlib/zlib_1.2.11.dfsg-2+deb11u2.dsc
dpkg-source -x zlib_1.2.11.dfsg-2+deb11u2.dsc

dget https://deb.debian.org/debian/pool/main/b/bzip2/bzip2_1.0.8-4.dsc
dpkg-source -x bzip2_1.0.8-4.dsc
```

Or use the copies already stored under `release/corresponding-source/`
and packed into `SOURCES/` of the public zip.
