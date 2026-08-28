# Corresponding source for public beta 0.1

See `SOURCE_INFO.md` at the repository root for the full provenance table.

This directory stores the **exact Debian source triples** that correspond to
the armhf binaries we redistribute, plus unpacked `debian/patches` where
the source package actually has patches.

**Not included:** any `libdvdcss` binary, `.deb`, or tarball. CSS remains a
user-supplied optional runtime at `/media/fat/DVD/lib/libdvdcss.so.2`.

FFmpeg 6.0.1 corresponding source is the upstream tarball copied into the
public zip as `SOURCES/ffmpeg-6.0.1.tar.xz` (hash in `SOURCE_INFO.md`).
Our player/launcher/rip C sources are copied as
`SOURCES/mister-dvd-player-arm/`.

| Binary we ship | Debian / upstream source |
|---|---|
| `libdvdread.so.8.0.0` SHA-256 `222de9bd…534c8` | `libdvdread` 6.1.1-2 triple |
| `libdvdnav.so.4.3.0` SHA-256 `2e113c2e…5980f` | `libdvdnav` 6.1.0-1 triple (binary is `6.1.0-1+b1` binNMU) |
| `dvdbackup` SHA-256 `fbfa1a1f…da320` | `dvdbackup` 0.4.2-4.1 triple |
| `genisoimage` SHA-256 `a114528e…7d8e` | `cdrkit` 1.1.11-3.2 triple |
| `libmagic.so.1.0.0` + `magic.mgc` | `file` 5.39-3+deb11u1 triple |
| `libz.so.1.2.11` | `zlib` 1.2.11.dfsg-2+deb11u2 triple |
| `libbz2.so.1.0.4` | `bzip2` 1.0.8-4 triple |

`libdvdread-6.1.1-2-debian-patches/` and `dvdbackup-0.4.2-4.1-debian-patches/`
are unpacked `debian/patches/` copies so the patches are readable without
unpacking the debian tarball.

`libdvdnav` 6.1.0-1 has **no** `debian/patches`.

Publish these files with the binary zip. Do not substitute a VideoLAN
tarball for a Debian triple. Do not substitute a SourceForge `dvdbackup`
`.tar.gz` for the Debian 0.4.2-4.1 triple (that binary includes
`libdvdread6.1.0.patch`).
