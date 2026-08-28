# Public beta 0.1 — compliance report

Date: 2026-08-28  
RC freeze: `bf9197036a526e409ca22a869a6ba03e2fa65849`  
Distribution model: **no `libdvdcss` binary**; **Rip DVD to USB included**  
Playback / HDMI / navigation / buffering / MiSTer_DVD / launcher UI / font:
**not changed**. No FPGA, player, or MiSTer_DVD rebuild.


## 1. Exact public package contents

Staged by `release/pack_public_beta.sh` as `MiSTer_DVD_Player_0.1.0-public-beta`.

### Binaries (hash-gated)

| Path in package | SHA-256 | Licence | Corresponding source | Licence text |
|---|---|---|---|---|
| `MiSTer_DVD_Player.rbf` | `3d841dc03aacfd58e21de8a7f0721d45bbfbf0c5d0b48bed760fa270c74c24f5` | GPL-2-or-later HDL + vendor PLL IP | this repo `fpga/` at `1e716f1` | `LICENSES/FPGA-COPYING.GPLv2` |
| `MiSTer_DVD` | `846eb22318bc6fd59c72e766daba6f9b40179b5bd231e358b8a9bfb89b8ade84` | GPL-3 | Main `0a8fb44` + `main-dvd/` overlay | `LICENSES/Main_MiSTer-COPYING.GPLv3` |
| `DVD/dev/dvd_av_threaded_test` | `89ed3327df140d98fc5e2c7edcf6312d798f8ed50ba82aa70abe3404be0df09c` | GPL-2.0-or-later (our source) + LGPL-2.1+ FFmpeg + GPL-2+ dvdnav/dvdread | player source + FFmpeg 6.0.1 tarball + Debian libdvd* triples | `COPYING.GPLv2`, `FFmpeg-*`, Debian copyrights |
| `DVD/dev/dvd_launcher` | `f4fea091a7fdb53200cb5ab9a65c21d051f127ea6fd6e0e6aa0c01f0ce39e58f` | GPL-2.0-or-later | `dvd_launcher.c` + libdvdread Debian triple | `COPYING.GPLv2`, `libdvdread.copyright` |
| `DVD/dev/dvd_rip_iso` | `42aec76ee04e6eb5b70c962ab764a6eeed897f2876cfc45cb0a39b39df51cecf` | GPL-2.0-or-later | `player/tools/dvd_rip_iso.c` | `COPYING.GPLv2` |
| `DVD/dev/dvdbackup` | `fbfa1a1f4750a88a59989cd60bc40820fbd61e3b09f29f0738849cb78e6da320` | GPL-3+ | Debian `dvdbackup` 0.4.2-4.1 | `dvdbackup.copyright`, `COPYING.GPLv3` |
| `DVD/dev/genisoimage` | `a114528e04c5262387b358baebc252e31b2be8de4df4e87ca130ee35ac3d7d8e` | GPL-2 | Debian `cdrkit` / `genisoimage` 9:1.1.11-3.2 | `genisoimage.copyright`, `COPYING.GPLv2` |
| `DVD/dev/magic.mgc` | `5f7ada0953e39fa859e15eff4ac3f3fc2064cac23769019d6aac7b53cb2c9c54` | BSD-2-Clause-alike (file/libmagic) | Debian `file` 1:5.39-3+deb11u1 (`libmagic-mgc`) | `libmagic.copyright` |
| `DVD/lib/libdvdread.so.8.0.0` (+ `libdvdread.so.8`) | `222de9bd1f40c01514f8dd88fa30bb669aaae4c09959dbf26dc15061cff534c8` | GPL-2+ | Debian `libdvdread` 6.1.1-2 | `libdvdread.copyright` |
| `DVD/lib/libdvdnav.so.4.3.0` (+ `libdvdnav.so.4`) | `2e113c2e65911713778537947644940278928d2eff3f04390fcafd4aef15980f` | GPL-2+ | Debian `libdvdnav` 6.1.0-1 (`+b1` binNMU) | `libdvdnav.copyright` |
| `DVD/dev/rip-lib/libmagic.so.1.0.0` (+ `libmagic.so.1`; also copied under `DVD/lib/`) | `a9693bd66fe1782b5d02043e243dd56e73897d3d4c3a4b17007c4313b9ca1e60` | BSD-2-Clause-alike | Debian `libmagic1` 1:5.39-3+deb11u1 | `libmagic.copyright` |
| `DVD/dev/rip-lib/libz.so.1.2.11` (+ `libz.so.1`; also copied under `DVD/lib/`) | `e8f5ff20a6dd792bef144d2f3280c62189279f17c0c41db6a32cf7c8534c681a` | Zlib | Debian `zlib1g` 1:1.2.11.dfsg-2+deb11u2 | `zlib.copyright` |
| `DVD/dev/rip-lib/libbz2.so.1.0.4` (+ `libbz2.so.1` / `libbz2.so.1.0`; also copied under `DVD/lib/`) | `6b815fd378d384641e01950b222f86f2dcb163f0ea80afffced4ee5a564d0120` | bzip2 BSD-variant | Debian `libbz2-1.0` 1.0.8-4 | `bzip2.copyright` |

FFmpeg is not a separate binary; it is inside the player. Tarball
`SOURCES/ffmpeg-6.0.1.tar.xz` SHA-256
`9b16b8731d78e596b4be0d720428ca42df642bb2d78342881ff7f5bc29fc9623`.

Also in the zip: installer/uninstaller, `LICENSES/`, `LICENSING.md`,
`THIRD_PARTY_NOTICES.md`, this report, `SOURCES/` (Debian triples, FFmpeg
tarball, our ARM/Main overlay sources), `MiSTer_DVD_Player_CHECKSUMS.txt`.

**Absent (do not ship):** `libdvdcss.so`, `libdvdcss.so.2`,
`libdvdcss.so.2.2.0`, any `libdvdcss2` `.deb`, any libdvdcss tarball.
Not copied merely because they existed in the private-beta tree:
cdrkit extras (`isoinfo`, `mkzftree`, …), `libdvdcss`.

SS1 already provides `libc`, `libpthread`, `ld-linux-armhf`; those are
not copied.


## 2. Rip DVD runtime chain (verified)

Launcher still shows **Rip DVD to USB** and `execl`s `dvd_rip_iso`.
`dvd_rip_iso` runs `dvdbackup -M` then `genisoimage -dvd-video`, with
`LD_LIBRARY_PATH=$selfdir:$selfdir/rip-lib:/media/fat/DVD/lib:/media/fat/DVD/dev/rip-lib`
and `MAGIC=$selfdir/magic.mgc`.

ELF `NEEDED` (actual public ripping path):

| Binary | NEEDED |
|---|---|
| `dvd_rip_iso` | `libdvdread.so.8`, `libc.so.6` |
| `dvdbackup` | `libdvdread.so.8`, `libc.so.6`, `ld-linux-armhf.so.3` |
| `genisoimage` | `libmagic.so.1`, `libz.so.1`, `libbz2.so.1.0`, `libc.so.6`, `libpthread.so.0` |

`libmagic`, `zlib`, `bzip2`, and `magic.mgc` **are** required (genisoimage).
`dvdbackup` does **not** need them. Neither rip binary has `libdvdcss` as
ELF `NEEDED`; libdvdread `dlopen`s CSS if present.

Hashes of `player/dist` files match the Debian armhf `.deb` payloads listed
in `SOURCE_INFO.md`.


## 3. CSS policy

- Not packaged. Optional user-supplied
  `/media/fat/DVD/lib/libdvdcss.so.2`.
- Installer does not `die` when CSS is missing; does not download CSS;
  does not delete or overwrite an existing CSS library.
- Without CSS: unencrypted physical DVDs remain usable where supported;
  ISO playback remains available; encrypted physical playback/ripping may
  be unavailable.
- `release/verify_no_libdvdcss.sh` fails the public pack if a CSS binary
  is staged.
- `release/verify_conservative_css_policy.sh` checks installer policy
  without SS1 hardware.


## 4. Font8x8 provenance

`player/tools/dvd_launcher.c` embeds `font8x8[95][8]` (ASCII 32–126).
**Glyph bytes, comment, and renderer are unchanged.** The launcher was
not rebuilt for licensing.

Investigation:

| Source checked | Result |
|---|---|
| Git history | Table introduced in `0c8b442` (2026-08-26, “add launcher, library and autostart”). No prior copy in this repo. |
| File comment at introduction | Only: `/* 8x8 glyphs for ASCII 32..126. Row-major, MSB (bit 7) = leftmost pixel. */` — no author, URL, or licence. |
| Later commits | Bit-order renderer fix only; table bytes not replaced. SPDX line added later; comment was briefly annotated then **reverted** to the original wording. |
| Filenames | No `font8x8.h` / `font8x8_basic.h` in-tree. |
| Byte-pattern / local source | Only match is `dvd_launcher.c` itself. |
| Marcel Sondaar / dhepper `font8x8_basic.h` | **Not a match** (`!` and `%` glyphs differ). |
| Common ESP32 `font8x8.h` | Differs on `%`. |
| Agent notes | Earlier chat described packing as “classic font8x8” meaning **MSB-first bit order**, not a cited upstream file. |

**Status: `font8x8 provenance unresolved`**

That is recorded here. It is **not** treated as a reason to modify or
rebuild the launcher.


## 5. Remaining compliance issues

Recorded, not used to change binaries:

1. **`font8x8 provenance unresolved`** (section 4). Continue investigation
   only; do not replace the font for 0.1.
2. **Counsel items unchanged:** CSS circumvention if a user later adds CSS;
   MPEG/DTS patents; MiSTer/DVD trademarks; Quartus/Altera PLL IP in the
   RBF.
3. **SPDX comments vs frozen hashes:** player/launcher **source** now has
   SPDX lines; RC binaries were not rebuilt. Public 0.1 ships the frozen
   hashes in `RELEASE_CANDIDATE_0.1.md`.
4. **FPGA / full Main tree** are not duplicated inside the zip (size).
   Commits are recorded; overlay sources are in
   `SOURCES/mister-dvd-player-arm/`. Offer the git tree as the rest of
   corresponding source.
5. **End-user docs** (repo root, not rebuilt into the frozen zip):
   `README.md`, `INSTALL.md`, `LEGAL.md`, `PRIVACY.md`, `CREDITS.md`,
   `CHANGELOG.md`, `KNOWN_ISSUES.md`, `RELEASE_NOTES_0.1.md`.

Non-blockers completed: licence map, `LICENSES/` including the ripping
stack, Debian corresponding-source triples for dvdread/dvdnav/dvdbackup/
cdrkit/file/zlib/bzip2, FFmpeg tarball in the zip, CSS-optional installer,
public pack CSS gate, verified rip ELF dependency chain.


## 6. Zip creation

Nothing besides **documentation/packaging** (and the presence of the
already-frozen RBF at `PUBLIC_RBF`) blocks creation of the public beta
zip. Do not rebuild FPGA, player, launcher, or MiSTer_DVD for these
licensing decisions.
