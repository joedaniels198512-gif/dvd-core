# Audio queue 128-packet experiment

This is a diagnostic build, not a hardware-verified playback fix.

Base: `5bc8c57763961c3d0a27138fbf2f1c1ea75f3bdb`.
Branch: `experiment/audio-queue-128`.
The only runtime source change is `AUDIO_Q_CAP` from 32 to 128 in
`player/tools/dvd_av_threaded_test.c`. Its introductory comment is updated too.
Video queue limits, prefill, audio conversion, timing, and HOL logic are unchanged.

## Build and checks

Built on Apple Silicon with the repository's player build recipes and:

- `arm-unknown-linux-gnueabihf-gcc` 15.2.0, crosstool-NG 1.28.0
  (messense macOS cross-toolchains v15.2.0, generic ARM hard-float variant).
- `-march=armv7-a -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard`.
- FFmpeg 6.0.1, using the configure options in `player/build_mac.sh`.
  Tarball SHA-256: `9b16b8731d78e596b4be0d720428ca42df642bb2d78342881ff7f5bc29fc9623`.
- DVD headers from the repository's libdvdnav 6.1.0 and libdvdread 6.1.1
  source archives; linked DVD libraries from the official beta.2 package,
  verified against the hashes in its corresponding-source document.
- Local wrapper scripts only adjust dependency/output paths in the existing
  recipes. They do not change compiler options or playback source.

Both the 128-packet experiment and a freshly compiled 32-packet control passed
ELF/disassembly checks: ARM ELF32 hard-float, VFPv3, NEONv1,
`/lib/ld-linux-armhf.so.3`, and no VFPv4 fused instructions. Both have the same
dynamic dependencies and required symbol-version set as the official beta.2
player (highest GLIBC requirement 2.28, no libstdc++ dependency).
Build warnings concern ignored `gcc_struct` attributes in upstream DVD headers
and overlapping architecture/CPU flags. No source workarounds were applied.

No MiSTer execution or DVD playback was performed during compilation.

## Install and compare

Use an existing working DVD Player beta.2 installation. Exit DVD playback and
return to the MiSTer menu before replacing the binary.

1. Back up `/media/fat/DVD/bin/dvd_av_threaded_test` to your computer.
2. Copy `experiment/DVD/bin/dvd_av_threaded_test` from the test package to
   `/media/fat/DVD/bin/dvd_av_threaded_test`, replacing only that file.
3. If necessary, run `chmod +x /media/fat/DVD/bin/dvd_av_threaded_test`.
4. Launch DVD Player normally and repeat the previously failing sequence.
   Save `/media/fat/DVD/logs/player.log` before another run overwrites it.
5. For the controlled comparison, exit playback and install the binary from
   `control/DVD/bin/dvd_av_threaded_test` at the same destination. Repeat the
   same disc/title/chapter and settings and save that log separately.
6. Restore the original backed-up binary after testing if desired.

No RBF, Main, supervisor, shared libraries, or configuration replacement is needed.
The freshly built control separates queue effects from rebuild/toolchain effects;
it is not the original release binary.

Test the failing main feature for at least 5 minutes (not just startup), a working
stereo title, menus, chapter skip, and pause/resume. Keep subtitle and A/V settings
the same. The experiment should report `aq=.../128`; the control should report
`aq=.../32`. Compare consecutive `apop`, `vpush`, `audio_fill`, `aclk`, `vq`, `yuv`,
`demux_wait`, stale-drop and underrun counters. Record whether freezes disappear,
occur later, or stay the same, and whether audible gaps or sync drift occur.
If the larger queue simply fills to 128 and stalls, it has delayed rather than
resolved the underlying blockage.
