# Known issues — v0.2.0 beta 1

Only current limitations. Fixed historical bugs are not listed.


## Beta software

This is an experimental community build. Discs, drives, and TVs differ.
Do not treat it as a finished commercial DVD player. Please file
compatibility reports with the GitHub issue form. Do not upload
commercial DVD ISOs, VOBs, decrypted disc contents, or CSS keys.


## CSS-encrypted physical DVDs

libdvdcss is **not** in the package. Encrypted physical playback may
fail until you supply a compatible
`/media/fat/DVD/lib/libdvdcss.so.2`. Unencrypted discs and ISO playback
do not require this project to ship CSS. See [INSTALL.md](INSTALL.md)
and [LEGAL.md](LEGAL.md).


## Native CRT aspect

HDMI / scaler automatically follows authored 4:3 or 16:9. The native
analogue CRT raster is not stretched or resampled. The analogue
connection does not tell the television to switch aspect; use the TV’s
own 4:3 / 16:9 control if needed.


## Startup clock join

When a title starts, video can run briefly before audio becomes the
master clock. You may see a short drop or correction of buffered frames.
That startup join is known; it is not treated as a release blocker.


## NTSC physical discs

NTSC **ISO** playback has been verified. PAL physical discs have been
tested. **NTSC physical DVD** drive behaviour is still thinly reported.


## What this beta is not

- Not a general media player (no MKV / H.264 / Xvid files)
- Not a loose `VIDEO_TS` folder player (ISO or physical disc)
- Multi-angle runtime control is not implemented; multi-angle discs play
  their default angle
- The short tail of some authored transition segments can be truncated
- Progressive HDTV / deinterlacing improvements are future work
- Some submenu / highlight follow-up remains open


## Developer / compliance note (not an end-user symptom)

The 8×8 glyph table used by MiSTer OSD has **unresolved provenance**
(`font8x8 provenance unresolved`). Glyphs were not changed for this
beta. Details: [COMPLIANCE_PUBLIC_BETA_0.1.md](COMPLIANCE_PUBLIC_BETA_0.1.md).
