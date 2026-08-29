# Release checklist

Use this right before a public release. It is **documentation only** — do not
change runtime code from this branch. The release-critical branch handles the
actual rename and behaviour changes.

## 1. Naming

Final public core name is intended to return to **DVD Player**.

- [ ] Final public core name: **DVD Player**
- [ ] Final internal `SETNAME`: **`DVD-Player`**
- [ ] Final user ISO folder:
  - SD: `/media/fat/games/DVD-Player/`
  - USB: `/games/DVD-Player/`

> Note: do NOT silently change runtime code to this naming in the release-prep
> branch. The rename is owned by the release-critical branch. This checklist
> only records the intended target so it can be verified.

## 2. Installation

- [ ] Installer/package creates `/media/fat/games/DVD-Player/`
- [ ] Does **not** automatically create empty USB `/games/DVD-Player/` folders
- [ ] Documents the USB folder correctly
- [ ] Lets normal MiSTer USB priority work as usual
- [ ] Does **not** move or copy existing ISOs
- [ ] Does **not** overwrite unrelated MiSTer files
- [ ] Required permissions are correct
- [ ] Clean install tested on a fresh SD

## 3. Playback regression

- [ ] Physical DVD autoplay
- [ ] Physical eject handling
- [ ] OSD "Play ISO..."
  - [ ] SD ISO
  - [ ] USB ISO
  - [ ] ISO filenames with spaces
- [ ] ISO → ISO switching
- [ ] Physical DVD → ISO switching
- [ ] Menus / navigation
- [ ] Subtitles
- [ ] PAL
- [ ] NTSC
- [ ] 4:3
- [ ] 16:9
- [ ] Authored-menu audio changes
- [ ] Runtime audio switching
- [ ] Hold CANCEL/B behaviour (return to launcher)

## 4. Release artifacts

- [ ] Correct RBF
- [ ] Correct Main binary
- [ ] Correct player binary
- [ ] Hashes recorded
- [ ] License files present
- [ ] README current
- [ ] Known issues current
- [ ] No debugging/test binaries accidentally shipped
- [ ] No `libdvdcss` shipped (if current licensing/distribution policy remains
      to omit it)
- [ ] No copyrighted test media included

## 5. GitHub

- [ ] Release tag created
- [ ] Release notes written
- [ ] Assets uploaded
- [ ] Issue Forms visible (bug / compatibility / feature)
- [ ] Compatibility form visible
- [ ] Links work
- [ ] Default-branch documentation current
