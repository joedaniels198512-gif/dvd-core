# README release TODOs

The README was **audited but intentionally not rewritten** in the
`feature/github-release-prep` branch. The release-critical branch is still
changing aspect-ratio handling, runtime audio switching, and the final core
naming, so writing final feature claims now would risk conflicting with that
work.

Update the following README sections **once the final build is validated**.
Each item lists the section, the current stale/missing content, and what it
should become.

## 1. Architecture description (README lines ~23–25)

- Current text: "MiSTer switches to a core-specific Main binary (`MiSTer_DVD`)
  that starts the launcher automatically."
- Stale: describes a **launcher-only** flow. The current/target design uses
  physical-DVD autoplay plus OSD-based ISO selection.
- TODO: describe the actual final flow (autoplay on a physical disc; OSD
  "Play ISO..." for ISOs) once finalised. Confirm the final Main binary name
  and core name against the release build.

## 2. Naming (README "Install" section, lines ~63–66)

- Current text: `[DVD-Player]` / `main=MiSTer_DVD`.
- The final public core name is intended to return to **DVD Player** with
  internal `SETNAME` **`DVD-Player`**. Confirm the exact `MiSTer.ini` entry and
  core/main names against the final release build before publishing.

## 3. ISO folder guidance (missing)

- Current: README says ISOs play "from the SD card or USB" but gives **no
  folder path**.
- TODO: document the final ISO folders once confirmed:
  - SD: `/media/fat/games/DVD-Player/`
  - USB: `/games/DVD-Player/`
- Also clarify how normal MiSTer USB priority applies.

## 4. OSD ISO selection (missing)

- Current: "Basic controls" describes a launcher with **Play Physical DVD /
  DVD Library / Rip DVD to USB** items only.
- TODO: document **OSD "Play ISO..."** selection (SD and USB, filenames with
  spaces) as the ISO entry point if that is the final design. Reconcile the
  "DVD Library" launcher item with the final OSD flow.

## 5. Testing status + playback samples (README lines ~28–39)

- Current: PAL physical / NTSC ISO figures for public beta 0.1.
- TODO: refresh with the final validated hardware results. **Do not** assert
  final PAL/NTSC or aspect results until the aspect/audio build is validated.

## 6. Controls — audio and aspect (README lines ~83–99)

- Current: "Select = Next audio track"; OSD lists TV Mode / CRT / A/V Sync.
- Runtime audio switching is an active release blocker and automatic
  widescreen/aspect detection is being reworked.
- TODO: confirm the final audio-switch control and the final aspect/OSD
  behaviour before finalising this section.

## 7. Known limitations link (README lines ~112–118)

- TODO: re-sync the README summary with the final `KNOWN_ISSUES.md` once the
  audio-swap and aspect-detection items have final status.

---

Until these are done, prefer linking to `KNOWN_ISSUES.md`,
`COMPATIBILITY.md`, and `docs/RELEASE_CHECKLIST.md` rather than restating
behaviour in the README.
