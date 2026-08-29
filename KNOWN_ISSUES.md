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


## Parked items and future work

These are known limitations that are not yet addressed and are not treated as
release blockers on their own:

- **Multi-angle runtime control** is not yet implemented/tested for release.
  Multi-angle DVDs will still play their default angle.
- **Short authored transition-tail truncation.** The very end of some short
  authored transition segments can be truncated. This is parked.
- **Progressive HDTV / deinterlacing improvements** remain future work.
- Some **DVD submenu / highlight-interaction follow-up** remains parked.


## Being actively worked on (status not final)

> The following items are under active development on a separate release
> branch. Their descriptions here are **placeholders** and must be updated with
> the final, validated behaviour before public release. Do not treat the text
> below as the final release state.

- **Runtime audio-track switching:** A desync that can occur when switching
  audio tracks during playback is currently being worked on as a release
  blocker. Final status is not yet determined. _(Placeholder — update once the
  fix branch is complete.)_
- **Automatic widescreen (aspect) detection:** Aspect-ratio handling is being
  reworked separately. The current beta behaviour should not be documented as
  the final release behaviour. _(Placeholder — update once the aspect work is
  validated.)_


## Developer / compliance note (not an end-user symptom)

The launcher 8×8 glyph table has **unresolved provenance** (`font8x8
provenance unresolved`). Glyphs were not changed for 0.1. Details:
[COMPLIANCE_PUBLIC_BETA_0.1.md](COMPLIANCE_PUBLIC_BETA_0.1.md).
