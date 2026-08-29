# GitHub labels

Recommended label set for triaging DVD Player issues and pull requests.

These are documentation only. This project does not currently ship a script or
API workflow to create labels automatically, so create them by hand in the
GitHub repository settings (or via `gh label create`) as needed. If the
repository already has suitable labels, reuse those instead of adding
duplicates.

Priority/severity labels are intentionally left out for now to avoid
over-engineering triage before there is a real issue volume.

| Label | Purpose |
|---|---|
| `bug` | A defect or incorrect behaviour in the DVD Player. |
| `disc-compatibility` | A DVD compatibility report for a specific title. |
| `feature-request` | A request for new functionality or an improvement. |
| `needs-log` | Waiting on a player log or more detail from the reporter. |
| `confirmed` | Reproduced or otherwise verified by a maintainer. |
| `installation` | Install, setup, packaging, or first-run/startup issues. |
| `video` | Video output, colour, scaling, or picture problems. |
| `audio` | Audio output, tracks, or decoding problems. |
| `av-sync` | Audio/video synchronisation problems. |
| `menus-navigation` | DVD menu rendering and navigation behaviour. |
| `subtitles` | Subtitle rendering or selection. |
| `controls` | Controller/input mapping and responsiveness. |
| `osd-iso` | OSD and the ISO browser / ISO selection. |
| `physical-drive` | Physical optical drive behaviour (detection, eject, read). |
| `performance` | Frame timing, underruns, or general performance. |
| `pal` | PAL-specific reports. |
| `ntsc` | NTSC-specific reports. |
