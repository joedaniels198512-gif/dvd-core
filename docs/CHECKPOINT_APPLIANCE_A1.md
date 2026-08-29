# Appliance A1 known-good checkpoint

Hardware-validated parallel Appliance core. Physical DVD autoplay only.
The original launcher DVD Player is untouched.

## Git

- Tag: `dvd-player-appliance-a1-known-good`
- Branch: `checkpoint/appliance-a1-known-good`
- Based on player checkpoint `162f894` / `dvd-player-test9.3.1-known-good`

## Identities

| Core | CONF_STR | Main | ARM |
|---|---|---|---|
| Old (frozen) | `DVD-Player` | `/media/fat/MiSTer_DVD` | `/media/fat/DVD/dev/dvd_launcher` |
| Appliance A1 | `DVD-Player-Appliance` | `/media/fat/MiSTer_DVD_Appliance` | `/media/fat/DVD_Appliance/bin/dvd_appliance` |

## Artifacts

| Item | SHA-256 |
|---|---|
| Package `release/test-builds/MiSTer_DVD_Player_appliance-a1-isolated.zip` | `540267e32965fbae2e3bda3f6143ecb1d76824322adc2c054d16ef72b7f515b2` |
| Appliance RBF | `26e682519407d1d941bfdf3a41bbdb5672163924a49129679fb1e322273a7104` |
| `MiSTer_DVD_Appliance` | `ee5f9fc94af65472e9fe832000345bfe5904c11aea13a6b66952a0a7fc82d2d8` |
| Player `dvd_av_threaded_test` | `478c3bbed220522987d4e6af2e6bc384c9663dc525d4b8d129e99a9caca8022f` |

FPGA: GitHub Actions run 33249638079 (`raetro/quartus:17.0`). CONF_STR identity only.

## Hardware

- Appliance launches with no launcher UI
- Empty drive idles; insert physical DVD starts authored playback
- Eject stops playback; same disc does not relaunch until eject/reinsert
- Old launcher core remains independent
