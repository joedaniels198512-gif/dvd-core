# Appliance A2.2 known-good checkpoint

Hardware-validated Appliance: physical DVD autoplay, OSD Play ISO,
native MiSTer USB-priority HomeDir, ISO replacement, CANCEL/B reaping.

Do not modify this tag/branch. Rollback target before final-release
aspect/audio work.

## Git

- Tag: `dvd-player-appliance-a2.2-known-good`
- Branch: `checkpoint/appliance-a2.2-known-good`
- Player checkpoint: `162f894` / `dvd-player-test9.3.1-known-good`

## Identities (A2.2 — keep for rollback)

| Core | CONF_STR | Main | ARM |
|---|---|---|---|
| Old launcher (frozen) | `DVD-Player` | `/media/fat/MiSTer_DVD` | `/media/fat/DVD/dev/dvd_launcher` |
| Appliance A2.2 | `DVD-Player-Appliance` | `/media/fat/MiSTer_DVD_Appliance` | `/media/fat/DVD_Appliance/bin/dvd_appliance` |

ISO HomeDir: `games/DVD-Player-Appliance` (USB `/media/usb0/...` first).

## Artifacts

| Item | SHA-256 |
|---|---|
| Package `release/test-builds/MiSTer_DVD_Player_appliance-a2.2-osd-dispatch.zip` | `758726acd61c732b29b3b8cd04767c39bd65fdcbe9c534b3c972352e479fb357` |
| Appliance RBF | `3294b0128f7df7402c267e3df984d156a48e3b6a0569e646faff4a327be61a58` |
| `MiSTer_DVD_Appliance` | `775dc21163bd18e30648bd6345558d78d0c1c935fd8de25c02a78b8213014ca9` |
| Supervisor `dvd_appliance` | `7bddfcfe31fceb5ad34241222154720be1596335142bad67a3125bd5f0581f58` |
| Player `dvd_av_threaded_test` | `478c3bbed220522987d4e6af2e6bc384c9663dc525d4b8d129e99a9caca8022f` |

FPGA: A2 RBF (GitHub Actions 33252703487). No A2.2 HDL change.

## Hardware

- Play ISO... intercepts at FILE_SELECT2; FIFO PLAY_ISO; no user_io_file_tx
- USB `/media/usb0/games/DVD-Player-Appliance/` preferred
- Filenames with spaces
- ISO→ISO and physical→ISO replacement
- Old launcher untouched
