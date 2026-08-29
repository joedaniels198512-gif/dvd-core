# Corresponding source — MiSTer DVD Player v0.2.0-beta.2

This archive accompanies `MiSTer_DVD_Player_0.2.0-beta.2.zip`.
`libdvdcss` is not redistributed.

## Binary → source map

| Shipped binary | SHA-256 | Corresponding source in this archive |
|---|---|---|
| `DVD_Player.rbf` | `bb4b32252b253df15acabf8c297883a5f8e6ffb6dee156bc1b95d82a1fc3d1ac` | `SOURCES/mister-dvd-player-fpga/` |
| `MiSTer_DVD` | `a2f1d757c3fd8c3de906cc8c060bf2806fe0b6a1b0fca4b0c03b9dc3779699da` | `SOURCES/mister-dvd-player-arm/main-dvd/` overlay + Main_MiSTer commit in `UPSTREAM_COMMIT` (`0a8fb44ccec6d69c8b7f158abd5fe8065ab2bf4f`) |
| `DVD/bin/dvd_player` | `4bf081fe210481c1579cce8868f58dca3914aff3ad08f5bc9cad276dd0c30f35` | `SOURCES/mister-dvd-player-arm/dvd_appliance.c` and `build_dvd_appliance.sh` |
| `DVD/bin/dvd_av_threaded_test` | `0edb459eb255336e9d8a4c3e4979fef4de4d1e89dbd64f5d3200a196c4e1f00c` | `SOURCES/mister-dvd-player-arm/dvd_av_threaded_test.c` and `build_dvd_av_threaded_test.sh` |
| `DVD/lib/libdvdnav.so.4.3.0` | `2e113c2e65911713778537947644940278928d2eff3f04390fcafd4aef15980f` | `SOURCES/debian-libdvdnav/` (Debian `libdvdnav` 6.1.0-1 triple) |
| `DVD/lib/libdvdread.so.8.0.0` | `222de9bd1f40c01514f8dd88fa30bb669aaae4c09959dbf26dc15061cff534c8` | `SOURCES/debian-libdvdread/` (Debian `libdvdread` 6.1.1-2 triple) |
| FFmpeg 6.0.1 (static in the player) | tarball `9b16b8731d78e596b4be0d720428ca42df642bb2d78342881ff7f5bc29fc9623` | `SOURCES/ffmpeg-6.0.1.tar.xz` |

## FPGA / HDL

`SOURCES/mister-dvd-player-fpga/` is the Quartus 17.0 project used to produce
`DVD_Player.rbf` (`DVD.qpf` / `DVD.qsf` / `DVD.qip` / `DVD.sv` / `rtl/` /
`sys/`). Compile recipe: `BUILD.txt` in that directory (GitHub Actions
`raetro/quartus:17.0`, `quartus_sh --flow compile DVD.qpf`).

Quartus cache and outputs are not included (`db/`, `incremental_db/`,
`output_files/`). Simulation benches are not included.

## Debian / FFmpeg notes

Do not substitute a VideoLAN tarball for a Debian source triple.
`libdvdnav` 6.1.0-1 has no `debian/patches`; the binary is `6.1.0-1+b1` (binNMU).
See `SOURCE_INFO.md` and `LICENSING.md`.
