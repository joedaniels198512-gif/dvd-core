# Known issues — public beta 0.1

Only current limitations. Fixed historical bugs are not listed.


## Public beta

This is an experimental community build. Discs, drives, and TVs differ.
Do not treat it as a finished commercial DVD player.


## NTSC physical discs

**NTSC physical DVD playback has not been tested.** NTSC **ISO** playback
has been verified. PAL physical discs have been tested successfully.


## CSS-encrypted physical DVDs

libdvdcss is **not** in the package. Encrypted physical playback and
ripping may fail until you supply a compatible
`/media/fat/DVD/lib/libdvdcss.so.2`. Unencrypted discs and ISO playback
do not require this project to ship CSS. See [INSTALL.md](INSTALL.md)
and [LEGAL.md](LEGAL.md).


## Startup clock join

When a title starts, video can run briefly before audio becomes the
master clock. You may see a short drop or correction of buffered frames.
Settled playback on the freeze samples had **0** new underruns (PAL
physical feature and NTSC ISO). That startup join is known; it is not
treated as a 0.1 blocker.


## Ripping storage

Ripping needs a writable `/media/usbN` volume and about **twice the disc
size plus 512 MB** free. FAT32 cannot hold an ISO over 4 GB. An existing
`<title>.iso` of the same name is not overwritten. Encrypted discs may
need user-supplied libdvdcss. Do not remove the disc or USB stick during
a rip.


## What this beta is not

- Not a general media player (no MKV / H.264 / Xvid files)
- Not a loose `VIDEO_TS` folder player (ISO or physical disc)
- Multi-angle switching is not a documented, tested feature
- Movie subtitle overlay is enabled in the launcher (`--fpga-yuv420-subtitles`)
  but is **not** listed among the freeze’s confirmed hardware tests;
  authored menu highlights are part of DVD navigation support
- NTSC physical drive behaviour is unknown until someone tests a disc


## Developer / compliance note (not an end-user symptom)

The launcher 8×8 glyph table has **unresolved provenance** (`font8x8
provenance unresolved`). Glyphs were not changed for 0.1. Details:
[COMPLIANCE_PUBLIC_BETA_0.1.md](COMPLIANCE_PUBLIC_BETA_0.1.md).
