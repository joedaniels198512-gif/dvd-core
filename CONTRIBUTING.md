# Contributing

Thanks for helping improve the MiSTer DVD Player! This is a community project,
and clear reports and small, focused changes make a big difference.

## Reporting bugs

- Please use the **GitHub issue forms** (New issue → pick a template):
  - **Bug report** for something that is broken.
  - **DVD compatibility report** for how a specific disc behaves.
  - **Feature request** for new functionality.
- Include a **log whenever possible**. The current build writes logs under a
  DVD Player log directory on the SD card (the exact folder name is still being
  finalised for release, so check what exists on your system).
- Give **exact reproduction steps**, and note whether the problem happens every
  time and whether another DVD/ISO works.

## Compatibility reports are especially welcome

DVD authoring differs a lot between titles and regions, so structured
compatibility data is very useful. When reporting a disc, please include:

- The **DVD title** (and edition/release if you know it).
- The **region** and **PAL/NTSC** video standard.
- Whether you tested a **physical disc** or an **ISO**.
- What worked and what did not (menus, playback, audio, subtitles, chapters).

## Please do not upload copyrighted material

- Do **not** upload DVD ISOs, VOB files, or any decrypted disc contents.
- Do **not** upload CSS keys or decrypted commercial-media contents.
- Logs and screenshots are welcome; disc contents are not.

## Pull requests

- **Small pull requests are preferred.** They are easier to review and land.
- **Avoid bundling unrelated fixes** in one PR; open separate PRs instead.
- Explain the intent of the change and how you verified it.
- Changes to **playback timing, FPGA video, DVD navigation, or A/V sync**
  must explain **how they were hardware-tested** (which hardware, PAL/NTSC,
  physical disc vs ISO, and what you observed). These areas are timing- and
  hardware-sensitive and cannot be validated from code review alone.

Thanks again for contributing.
