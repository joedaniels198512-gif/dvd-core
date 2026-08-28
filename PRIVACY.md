# Privacy — MiSTer DVD Player

This software does not provide an account, telemetry service, analytics
backend, advertising system, or cloud service.


## What the player software does

The public-beta **player, launcher, and ripping helper** are
local programs on the SuperStation One / MiSTer. In the source used for
this release there is **no** application-level telemetry, crash reporter,
or analytics client.

Those programs do **not** intentionally collect or transmit personal
data. They read a DVD or ISO, decode audio/video, write local logs and
(if you rip) an ISO on USB.

Checked in this tree for this documentation:

- Player, launcher, library, and rip helper C sources: no sockets,
  HTTP clients, or analytics calls
- Custom Main overlay: no network client
- Packaging/install docs do not download packages or contact VideoLAN

FFmpeg is built into the player with a minimal demux/decode set (MPEG-PS
and related DVD codecs), not as a networked media client.


## What this document does not cover

Linux, MiSTer firmware, your network, SSH, and websites you open
yourself (including GitHub and VideoLAN pages linked from these docs)
are outside this software. This file does not describe their privacy
practices.

Local logs under `/media/fat/DVD/logs/` stay on the SD card unless you
copy them off.
