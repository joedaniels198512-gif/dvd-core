# Test 9 MEDIA RESET — parked

Parked 2026-08-29 on branch `test9-media-reset-parked`.

Do **not** merge this into menu/SPU/HLI work. Do **not** rebuild Test 9
unless resuming this experiment. Joe's SS1 stays on **Test 7** until Anime
is available again (their disc is the decisive reproduction).

---

## Purpose

Test 7 showed apparent multi-second A/V offsets immediately after same-domain
VTSMenu/VMGM SOFT hops.

Current leading diagnosis:

old compressed/decoded audio and MPEG-PS parser state survives a SOFT hop
while new-cell video restarts its PTS timeline.

Especially suspicious (Anime, `nav_gen=10` / `codec_gen=9`):

- old presentation still at `raw_vpts≈4.527 s` / `aclk≈4.577 s`
- then `AFRAME pts_us≈4.767 s`
- then new `VPKT pts_us=287267` and `APKT pts_us=5791267`
- `RAWPTS PACKET DELTA audio_minus_video_us=5504000`
- then `VIDEO PTS CLAMP` of new pictures onto the leftover timeline

Test 9 is intended to test whether the apparent 5.504 s offset is **stale
cross-hop media** rather than an authored A/V delay.

---

## Baseline

Test 9 must be based on **Test 7**, not Test 8.

Test 8 (shared-STC / AUDIO_FIRST / VIDEO_FIRST / audio park / join guard)
was a **failed experiment** and was reverted from the player source before
Test 9. Do not revive it here.

| Artifact | SHA-256 |
|---|---|
| Test 7 player | `e3786b05cf452f855b6aa3d6df0efe79b8a74e78eb4c676c89d6e2d9a8e05c73` |
| Test 7 ZIP | `d580e54b4d03d4443f4af1f4527d39514df87f75d5a54bab700fa3b8a28c7546` |
| Test 7 ZIP path | `release/test-builds/MiSTer_DVD_Player_rawpts-test7.zip` |

Keep:

- Test 4 HOL escape (soft 2560 / hard 4096, enter `aq<=16`, exit `aq>=24`)
- Test 5 VTS/HARD discontinuity safety (`hop_pending`, codec reopen, opaque=`codec_gen`)
- Test 6 MrAudio physical drain + first-video start gate
- Test 7 RAWPTS diagnostics

Do not change Test 4 thresholds, Test 5 HARD/VTS behaviour, Test 6 drain
mechanism, `assign_video_pts`, stale-drop, audio scheduling, stream
selection, SPU/HLI, or FPGA.

---

## Test 9 MEDIA RESET design

Same-domain VTSMenu/VMGM HOP only:

- preserve dvdnav VM/menu/button state
- preserve `codec_gen` and codecpar
- `nav_gen` acts as **media lifetime** generation (`codec_gen` 9→9, `nav_gen` 9→10)
- flush `aq`
- flush `vq` / YUV
- clear PCM hold
- stop/drain MrAudio using the Test 6 mechanism (audio thread)
- reset `first_audio_pts` and the audio clock
- clear video PTS assignment baseline (`first_genuine_pts` / `timeline_pts` / `assigned_pts`)
- audio decoder/SWR reset on the **audio** thread
- video parser/decoder media flush on the **video** thread
- one-shot `soft_media_boundary_pending`
- AVIO must never concatenate pre-hop and post-hop MPEG-PS bytes
- if pre-hop bytes are already in the AVIO fill, return those bytes only
- next AVIO call under the pending flag returns `AVERROR_EOF` before any new-cell bytes
- `avformat_flush()` runs **only** on the demux thread **after** `av_read_frame()` returns
- keep the same `AVFormatContext` / same AVIO / same VTS
- do **not** use Test 5 `hop_pending` VTS reopen (`vi`/`ai` = −1, stream rediscovery)
- re-arm Test 7 prefill
- re-arm Test 6 first-video audio gate (`first_video_presented_gen = 0`)

### AVIO / demux thread order (re-entrancy)

Never call `avformat_flush`, parser reset/free, or `AVCodecContext`
flush/free/reopen from inside the dvdnav/AVIO callback (same class as
Test 5 VTS_CHANGE).

1. Inside AVIO: detect SOFT hop → increment `nav_gen` → queue/clock/flag
   work → set `soft_media_boundary_pending` → old-only or EOF return
2. Active `av_read_frame()` returns
3. Demux thread: discard returned old-generation packet → drain `fmt->pb`
   → `avformat_flush(fmt)` → clear pending → resume `av_read_frame()`

### Packet-generation safety

Do **not** replace `codec_gen` stored in `AVPacket.opaque`.

Keep current Test 7 opaque semantics and `VQ_MARK_STILL_BOUNDARY`.

Add `nav_gen` sidecar metadata to `PktQ` (`uint32_t *nav_gens` next to
`AVPacket *pkts`). Do not pack generations into a pointer on 32-bit ARM.

At queue push:

- retain packet `codec_gen` in `opaque`
- stamp `player_nav_gen(p)` into that slot

At pop:

- return both codec generation (opaque) and stamped `nav_gen`
- use the **stamped** `nav_gen`, never `player_nav_gen(p)` sampled after pop

Reject SOFT-stale packet if:

`pkt_nav_gen != player_nav_gen()`

Keep existing HARD rejection independently:

`pkt_codec_gen != decoder_codec_gen` (opaque vs live `codec_gen`)

HARD also increments `nav_gen`, so both filters may reject. That is fine.

Queue flush must zero sidecar slots together with the `AVPacket` slots.

This protects packets already queued or popped across a SOFT media reset
without pretending the codec changed.

### In-flight races

Audio: drop popped packet if stamped `nav_gen` ≠ current; do not
`avcodec_send_packet`; do not write PCM/MrAudio; `audio_reset_req` runs
Test 6 drain + decoder/SWR on the audio thread.

Video: drop popped packet if stamped `nav_gen` ≠ current; abort mid-parse
if live `nav_gen` ≠ stamped `pkt_gen`; media-flush parser/decoder on the
video thread without bumping `codec_gen`; YUV slots already drop
`slot_gen != nav_gen`.

---

## Pass/fail criterion

On Anime's problematic same-domain menu hop:

If after MEDIA RESET:

- new VPKT ≈ 0.287 s
- new APKT ≈ 0.287 s / close to video

then the previous ~5.504 s delta was stale cross-hop audio.

If a clean new-media generation still produces:

- VPKT ≈ 0.287 s
- APKT ≈ 5.791 s

with no old AFRAME/packet surviving the hop, then the offset is genuinely
authored and needs a later **narrow scheduling** solution (not Test 8's
shared-STC architecture).

---

## Do not include in Test 9

- shared-STC Test 8 architecture
- global large-PTS discontinuity acceptance
- audio parking
- video lead gating
- fixed A/V offsets
- menu/SPU/HLI fixes
- FPGA changes

---

## Parked Test 9 build (already produced; do not rebuild to resume design)

Player only. VFPv4 gate **PASS**.

| Artifact | SHA-256 |
|---|---|
| Test 9 player | `44dcb173793a1625e882d1a1a2dbb21b74d971ccdfe6f17a7407faf54c0d3178` |
| Test 9 ZIP | `5073ae7d63c29b08dcdaae1253699174dbbf460d2982eb286f300135faad2fc9` |
| Test 9 ZIP path | `release/test-builds/MiSTer_DVD_Player_mediareset-test9.zip` |

ZIP contents: `dvd_av_threaded_test`, `SHA256.txt`, `TEST_BUILD.txt`.

Public beta freeze remains untouched (`c6fa2d34…` zip,
player `89ed3327…`).

---

## Current testing state

- Joe's SS1 should remain on **Test 7**.
- Test 9 waits for Anime: their disc is the decisive reproduction case.
- Menu investigation is a **separate workstream**. Do not mix it with this
  parked MEDIA RESET experiment.
