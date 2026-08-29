# Legal notices — MiSTer DVD Player

This is factual project documentation, not legal advice. Laws differ by
place. You are responsible for complying with the laws that apply to you.


## What this software is

MiSTer DVD Player is **unofficial, volunteer, open-source** software.
Public beta 0.2 is an experimental community build, not a finished
commercial DVD product.

It is **not** affiliated with, endorsed by, or sponsored by MiSTer,
MiSTerFPGA, SuperStation One, VideoLAN, FFmpeg, the DVD Forum, or any
content owner. Names are used only to identify hardware, software, and
the DVD-Video format.

Do not use official DVD logos or other trademarked logos with this
project unless you have permission from the trademark owner.


## Open-source licences

The public-beta zip redistributes original code and third-party
components under their own licences (GPL-2, GPL-3, LGPL-2.1, and others).
Details:

- [LICENSING.md](LICENSING.md)
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- [SOURCE_INFO.md](SOURCE_INFO.md)
- [LICENSES/](LICENSES/)
- [COMPLIANCE_PUBLIC_BETA_0.1.md](COMPLIANCE_PUBLIC_BETA_0.1.md)

Corresponding source for redistributed Debian binaries and FFmpeg is
shipped in the zip (`SOURCES/`) and kept in this repository.


## libdvdcss

**This project does not distribute libdvdcss.** The release zip does not
contain `libdvdcss.so.2`, `libdvdcss.so.2.2.0`, or any other libdvdcss
binary.

Encrypted commercial DVDs require you to obtain and install libdvdcss
separately. If you choose to do that, the runtime filename and location
this player uses are:

`/media/fat/DVD/lib/libdvdcss.so.2`

Unencrypted discs and ISO playback do not require it. The official
upstream project is VideoLAN libdvdcss:

https://www.videolan.org/developers/libdvdcss.html

CSS and anti-circumvention rules **vary by jurisdiction**. This document
does not state that providing or using libdvdcss is legal or illegal
where you live. Check local law.

This v0.2.0-beta.1 package does **not** include a DVD rip helper.


## Patents and other rights

MPEG-2, AC-3, DTS, and related formats may be covered by patents in some
places. Redistribution of this beta does not grant patent licences.
Treat that as your own compliance matter.
