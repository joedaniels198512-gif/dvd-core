# Public beta release candidate 0.1

Freeze date: 2026-08-28  
Branch: `exp/custom-main-dvd-player`  
Status: **source freeze** for the hardware-confirmed public beta candidate.  
Product behaviour is unchanged by this document.

This is **not** a rename of on-device files, not a new RBF, and not an
installer/package update. Publication, licensing, and documentation audit
are a later task.

Identify the freeze commit with:

```
git log -1 --format=%H -- RELEASE_CANDIDATE_0.1.md
```


## Confirmed binaries

| On-device path | SHA-256 |
|---|---|
| `/media/fat/DVD-Player_YUV_ResetFix_Test.rbf` | `3d841dc03aacfd58e21de8a7f0721d45bbfbf0c5d0b48bed760fa270c74c24f5` |
| `/media/fat/MiSTer_DVD` | `846eb22318bc6fd59c72e766daba6f9b40179b5bd231e358b8a9bfb89b8ade84` |
| `/media/fat/DVD/dev/dvd_av_threaded_test` | `89ed3327df140d98fc5e2c7edcf6312d798f8ed50ba82aa70abe3404be0df09c` |

Supporting binary (unchanged this freeze; required to reproduce the stack):

| On-device path | SHA-256 | Source |
|---|---|---|
| `/media/fat/DVD/dev/dvd_launcher` | `f4fea091a7fdb53200cb5ab9a65c21d051f127ea6fd6e0e6aa0c01f0ce39e58f` | `08c7f75622cc8d579af25fd6e944c74ac90cf9c3` (`player/tools/dvd_launcher.c`) |

Local trees that match the ARM binaries (not rebuilt for this freeze):

- `player/dist/dvd_av_threaded_test` → `89ed3327…`
- `main-dvd/bin/MiSTer_DVD` → `846eb223…`
- `player/dist/dvd_launcher` → `f4fea091…`


## Source commits that reproduce those binaries

| Binary | Commit | What it is |
|---|---|---|
| RBF `3d841dc0…` | `1e716f156a53d60856e394ead43ad10de5877674` | FPGA: restore idle-port YUV line issue; quiet-gate only new fills. Includes reset-safety parent `dbe950eff2132e4ddf7d7eed06c8773810899e3b`. |
| MiSTer_DVD `846eb223…` | `4a3a6fc3f1755b6972ebc5adeefc9fe7679e3355` | HDMI TMDS follows Monitor Sense (`main-dvd/apply_dvd_hooks.py` → `video.cpp`). Built on spawn-fix `0841c6fd89ea52718a4c223c68f6f0144d3a3f38`. |
| player `89ed3327…` | `89e9a793742d6d6f14b19942b1fef1e32416fb97` | Play Feature clock bootstrap + always-on nav/prefill diagnostics (`player/tools/dvd_av_threaded_test.c`). |

Upstream Main snapshot used by the custom overlay:

- `main-dvd/UPSTREAM_COMMIT` = `0a8fb44ccec6d69c8b7f158abd5fe8065ab2bf4f` (MiSTer-devel/Main_MiSTer)

`main-dvd/.src/` is gitignored. Reproduce MiSTer_DVD by cloning that upstream commit and running `main-dvd/apply_dvd_hooks.py` then `main-dvd/build_mister_dvd.sh`. Do not rebuild for this freeze.


## FPGA CI run that produced the RBF

| Item | Value |
|---|---|
| GitHub Actions run | [33175603689](https://github.com/joedaniels198512-gif/dvd-core/actions/runs/33175603689) |
| Workflow | `.github/workflows/build-core.yml` (`Build DVD core`) |
| Trigger | push of FPGA source on `exp/custom-main-dvd-player` |
| Head commit | `1e716f156a53d60856e394ead43ad10de5877674` |
| Quartus | Docker image `raetro/quartus:17.0` (v17.0.2.602) |
| CI artifact name | `DVD_FPGA_YUV420_DVDPlayer_Main.rbf` |
| On-device copy | `/media/fat/DVD-Player_YUV_ResetFix_Test.rbf` (same bytes; **do not rename on device in this freeze**) |

Do **not** start a new Quartus/FPGA compile to recreate this candidate.


## Build commands and toolchains

### RBF (already built; do not rebuild)

```
# GitHub Actions only — docker run raetro/quartus:17.0
quartus_sh --flow compile DVD.qpf
# artifact: DVD_FPGA_YUV420_DVDPlayer_Main.rbf
```

### MiSTer_DVD

```
cd main-dvd
./build_mister_dvd.sh
```

- Prefix: **`arm-none-linux-gnueabihf-gcc` GCC 10.2.1** (ARM GNU-A 10.2-2020.11)
- See `main-dvd/TOOLCHAIN.md`
- GLIBCXX max **3.4.28**
- **Never** Homebrew `arm-unknown-linux-gnueabihf-gcc` (GLIBCXX 3.4.32)
- **Never** `armv7-unknown-linux-gnueabihf-gcc` (VFPv4 / SIGILL)

### Player

```
# once: FFmpeg 6.0.1 static libs
player/build_mac.sh

# this binary
player/tools/build_dvd_av_threaded_test.sh
```

- Prefix: **`arm-unknown-linux-gnueabihf-gcc`** (Homebrew 15.2.0)
- Flags: `-march=armv7-a -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard`
- `objdump` VFPv4 gate in the build script (`vfma|vfms|vfnma|vfnms` ⇒ refuse)
- FFmpeg prefix: `player/.build/ffmpeg-ss1-cortex-a9-video`

### Launcher (unchanged)

```
player/tools/build_dvd_launcher.sh
```

Same Cortex-A9 / VFPv3 toolchain as the player. Does not link FFmpeg.


## FFmpeg and libraries currently shipped

Statically linked into `dvd_av_threaded_test` (configure in `player/build_mac.sh`):

| Component | Version | Notes |
|---|---|---|
| FFmpeg | **6.0.1** | `--disable-everything`; MPEG-PS / MPEG-1/2 / AC3 / E-AC3 / MP1–3 / DCA / pcm_dvd; **no `--enable-gpl`** |
| libdvdnav | **6.1.0** (`libdvdnav.so.4.3.0`) | VideoLAN / Debian armhf |
| libdvdread | **6.1.1** (`libdvdread.so.8.0.0`) | VideoLAN / Debian armhf |
| libdvdcss | **1.4.2** (`libdvdcss.so.2.2.0`) | VideoLAN |

Private-beta package hashes for those shared objects (still the shipped copies; installer was **not** changed):

| File | SHA-256 |
|---|---|
| `DVD/lib/libdvdcss.so.2.2.0` | `e72abca9000141b6e2dc04ec026d9f80efb5bfdf6de283871fed6fb362ffa4ac` |
| `DVD/lib/libdvdnav.so.4.3.0` | `2e113c2e65911713778537947644940278928d2eff3f04390fcafd4aef15980f` |
| `DVD/lib/libdvdread.so.8.0.0` | `222de9bd1f40c01514f8dd88fa30bb669aaae4c09959dbf26dc15061cff534c8` |

Also on device from the existing layout: zlib 1.2.11, bzip2 1.0.8, libmagic 5.39.


## Confirmed tests (this candidate)

- Daemon-free `[DVD-Player] main=MiSTer_DVD`
- Launcher auto-start
- No launcher zombies / duplicate flock exits
- Physical PAL DVD menus and feature playback
- CSS warmup
- PAL 720×576 playback: settled **24.99 fps vs 25.00** target
- PAL: **0** new underruns during settled sample
- NTSC 720×480 ISO playback: settled **29.95 fps vs 29.97** target
- NTSC: **0** new underruns during settled sample
- HDMI remains active through RGB→DVD playback transition
- CRT output works
- Authored DVD navigation works
- Feature clock-bootstrap deadlock fixed
- Stable A/V after startup join


## Known limitations

- **NTSC physical DVD has not been tested** (no physical NTSC disc available). NTSC ISO playback has been verified.
- Startup join still shows stale-frame / underrun counts that then freeze; **do not “optimise” that join** as part of this freeze.
- On-device filenames remain the experimental test names (`DVD-Player_YUV_ResetFix_Test.rbf`, `/media/fat/DVD/dev/…`). Publication rename is a later task.


## Files required for installation (current on-device layout)

Do **not** rename these in this freeze.

```
/media/fat/DVD-Player_YUV_ResetFix_Test.rbf
/media/fat/MiSTer_DVD
/media/fat/DVD/dev/dvd_launcher
/media/fat/DVD/dev/dvd_av_threaded_test
/media/fat/DVD/lib/libdvdcss.so.2
/media/fat/DVD/lib/libdvdcss.so.2.2.0
/media/fat/DVD/lib/libdvdnav.so.4
/media/fat/DVD/lib/libdvdnav.so.4.3.0
/media/fat/DVD/lib/libdvdread.so.8
/media/fat/DVD/lib/libdvdread.so.8.0.0
```

`MiSTer.ini` must contain:

```
[DVD-Player]
main=MiSTer_DVD
```

`dvd_autostart_daemon` must **not** be running. Stock `/media/fat/MiSTer` is not replaced.


## Protected known-good / public artifacts

Do **not** overwrite these accidentally (CI, deploy, or installer):

| Path / name | SHA-256 | Why |
|---|---|---|
| `/media/fat/MiSTer_DVD_Player.rbf` | `55e6b114bc34fe1d314ad7ec94e1944627968d44c42e947d13caba23470576ae` | Known-good public bitstream |
| `/media/fat/_Console/DVD_FPGA_YUV420_Production_RC1.rbf` | `677c42b928eae9c935d6cd9595433100f1a6edcef99f8dd41bc55f17498d7f03` | Strong pre-Main / RC1 candidate |
| CI / on-device name `DVD_FPGA_YUV420_Test.rbf` | do not overwrite | Known-good public artifact name; workflow must never reuse it |
| `/media/fat/DVD-Player_PreReset_YUV_AB.rbf` | `3b1ff8e18552d7e3ca2cb7ea698a54d3960a33eb7fa0a3361b49fea426b26bb8` | Pre-reset A/B; unsafe with `main=MiSTer_DVD` |
| `/media/fat/DVD_FPGA_YUV420_DVDPlayer_Main.rbf` (reset-safe, pre-idle-port) | `5554ad96a54282b211f204483e2fb45eeba9e412601b4d03b58edacea5de441e` | Do not confuse with ResetFix Test `3d841dc0…` |

Also do not overwrite stock `/media/fat/MiSTer`, the RC player/launcher/Main binaries listed above, or `MiSTer.ini` daemon-free `[DVD-Player]` selection as part of unrelated work.


## Out of scope for this freeze

- Do not rename current on-device files
- Do not build a new RBF
- Do not change installer/package
- Do not add features
- Do not optimise startup stale/underrun join behaviour
- Do not alter HDMI, clock, navigation, player buffering, or Main logic
