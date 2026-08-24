# DVD Player Core — Architecture

Target platform: **SuperStation One (SS1) / MiSTer**.

This document records the *proven* architecture and the measurements that drive
it. It is the source of truth for design decisions; update it whenever a new
measurement changes the plan.

---

## 1. Target hardware

| Property | Value |
|---|---|
| SoC | Intel/Altera Cyclone V (HPS + FPGA) |
| CPU | Dual-core ARM Cortex-A9, **ARMv7-A** |
| SIMD / FP | **NEON** + **VFPv3** (⚠️ *no* VFPv4 — no fused multiply-add) |
| DVD drive | Physical drive at `/dev/sr0` (proven working) |
| Display path | MiSTer FPGA video framework → HDMI (CRT later) |

### Toolchain (critical)

- **Correct cross compiler:** `arm-unknown-linux-gnueabihf-gcc`
- **NEVER use:** `armv7-unknown-linux-gnueabihf-gcc`
  Its runtime emitted VFPv4 `VFMA`/`VFMS`/`VFNMA`/`VFNMS` (fused) instructions,
  which the Cortex-A9 (VFPv3) does not implement → **SIGILL** on the SS1.
- Compile flags in use:
  `-march=armv7-a -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard`
- `player/build_mac.sh` includes a hard safety gate: it runs `objdump -d` on the
  final binary and **refuses to deploy** if any `vfma/vfms/vfnma/vfnms`
  instruction is present.

---

## 2. What is already proven

Component-level results validated on real SS1 hardware:

- ✅ Physical DVD drive works at `/dev/sr0`.
- ✅ `libdvdcss` / `libdvdread` / `libdvdnav` work (CSS + navigation).
- ✅ DVD navigation previously demonstrated (menus / titles).
- ✅ Native FFmpeg **MPEG-PS demux** works (custom AVIO fed by `dvdnav`).
- ✅ Native **MPEG-2 video decode** works.
- ✅ **AC-3 audio decode** works.
- ✅ Isolated **mailbox double-buffer video** (`working-double-buffer-video`).
- ✅ Isolated **MrAudio hardware-paced AC-3** (`working-audio-mraudio`).
- ✅ **Threaded A/V together** (`working-threaded-av`): demux + audio/video
  workers, audio master clock, mailbox presentation. See §7.

### Measured performance

| Operation | Cost | Notes |
|---|---|---|
| Optimized MPEG-2 decode | **~34.18 fps** | On a 25 fps PAL DVD → decode headroom exists |
| 720×576 YUV → BGR0 convert | **~3.5 ms/frame** | `swscale`, native resolution, no resize |
| **CPU scaling to display** | **~104 ms/frame** | ❌ **Not viable** — must not be used |

**Conclusion:** the ARM has ample budget to *decode* and do a native-resolution
colour conversion, but **must not perform final display scaling**. Scaling is the
FPGA's job.

---

## 3. Intended architecture

```
   DVD drive (/dev/sr0)
        │
        ▼
   ARM (HPS, Linux)
   ├─ libdvdnav / libdvdread / libdvdcss   (navigation + CSS)
   ├─ FFmpeg MPEG-PS demux (custom AVIO)
   ├─ MPEG-2 video decode  → YUV420P
   ├─ AC-3 audio decode
   └─ YUV → BGR0 at NATIVE DVD resolution (720×576 PAL / 720×480 NTSC)
        │            (~3.5 ms/frame — NO resizing on the ARM)
        ▼
   Shared framebuffer in DDR
        │
        ▼
   MiSTer FPGA video framework  (MISTER_FB → ASCAL scaler)
   ├─ scaling / aspect-ratio handling
   └─ video output
        │
        ▼
   HDMI  (CRT / analog later)
```

Key rule: **ARM decodes at native resolution; the FPGA scales.** No CPU scaling
ever ends up in the shipping path.

---

## 3a. MiSTer Template Baseline

`fpga/` was flattened into this repository from the official MiSTer core
template. The upstream provenance at the time of flattening:

| Item | Value |
|---|---|
| Upstream remote | `https://github.com/MiSTer-devel/Template_MiSTer.git` |
| Baseline commit SHA | `df59d675a1e11a2390fb0f6ba577909977380b7b` |
| Commit date | 2026-08-18 |
| Commit subject | `Update template with video sync always running (#109)` |

The nested `fpga/.git` was removed so `fpga/` is now normal content tracked by
the root `dvd-core` repository (not a submodule). To pull future framework
updates, compare against the upstream remote/commit above.

Local delta vs. upstream at flatten time: `Template.qsf` only differs by
`LAST_QUARTUS_VERSION` "17.0.2 Standard Edition" → "17.0.2 Lite Edition" (a
Quartus-written edition string; no functional change).

---

## 4. FPGA integration — prefer the existing MiSTer framework

**Before writing any custom framebuffer/scaler RTL, use the existing MiSTer
`MISTER_FB` / ASCAL infrastructure.** Do **not** modify files under `fpga/sys/`
unless later proven absolutely necessary (framework updates overwrite that dir).

The framebuffer interface the core receives (from `fpga/sys/emu_ports.vh`,
gated by `` `ifdef MISTER_FB ``):

| Signal | Dir | Purpose |
|---|---|---|
| `FB_EN` | out | Enable framebuffer readout |
| `FB_FORMAT[4:0]` | out | `[2:0]` bpp (110 = 32bpp), `[3]` 16-bit sub-mode, `[4]` 0=RGB/1=BGR |
| `FB_WIDTH[11:0]` | out | Frame width in pixels |
| `FB_HEIGHT[11:0]` | out | Frame height in pixels |
| `FB_BASE[31:0]` | out | DDR base address of the frame |
| `FB_STRIDE[13:0]` | out | Bytes per line (0 ⇒ rounded to 256B, else multiple of pixel size) |
| `FB_VBL` | in | Vertical blank from scaler |
| `FB_LL` | in | Low-latency indicator |
| `FB_FORCE_BLANK` | out | Force blank |

For our BGR0 / XRGB8888 32-bpp frames, the matching `FB_FORMAT` is
32bpp + BGR ⇒ `5'b1_0110` (`0x16`).

This matches the exact framebuffer layout the C player already validated on the
SS1's Linux `/dev/fb0`: 32 bpp, `R offset=16, G offset=8, B offset=0`
(little-endian XRGB8888 == byte order B,G,R,0 == "BGR0").

**Open integration question (to resolve before RTL work):** how the ARM shares
the decoded frame buffer with the FPGA `FB_BASE` region in DDR (reserved DDR
region + the HPS↔FPGA DDR bridge vs. the Linux `/dev/fb0` path currently used).
This is the first thing to investigate on the FPGA side; prefer the standard
MiSTer mechanism over anything custom.

---

## 5. Build & deploy (current, unchanged)

- **Build (Mac cross-compile):** `player/build_mac.sh`
  - Builds VFPv3-safe static FFmpeg once into `player/.build/ffmpeg-ss1-cortex-a9-video/`,
    then links `player/dist/dvdplayer_headless`.
  - Enforces the no-VFPv4 gate before producing the binary.
- **Deploy + run on SS1:** `player/deploy-test.sh`
  - `scp`s the binary to `root@192.168.1.212:/media/fat/DVD/dev` and runs it
    against `/dev/sr0` with `LD_LIBRARY_PATH=/media/fat/DVD/lib`.
- **On-device test helper:** `player/scripts/Headless_Test.sh`.

### FPGA build (do not run unless explicitly requested)

- Quartus **17.0.2** runs in Docker on Apple Silicon via Rosetta
  (image `quartus-mister-rosetta`).
- A full compile under Rosetta is **very slow** — do not start an FPGA compile
  unless explicitly asked.

---

## 6. Status of the current ARM program

`player/src/dvdplayer_headless.c` is the latest working ARM implementation. It is
a **staged diagnostic** (v0.9) with a `SIGILL` handler that reports the exact
stage + ARM PC/LR on an illegal instruction. It currently runs in a
**benchmark mode**: it converts YUV→BGR0 at native resolution and deliberately
does **not** copy to `/dev/fb0`, to isolate conversion cost (the ~3.5 ms/frame
figure). Framebuffer open/blit/restore code is present and validated but bypassed
in the benchmark path.

The next functional milestone is to move from this headless benchmark toward
handing native-resolution frames to the FPGA framebuffer path described in §4.

---

## 7. Proven threaded A/V (tag `working-threaded-av`)

Isolated video (`working-double-buffer-video`) and isolated audio
(`working-audio-mraudio`) were combined on SS1 with **one demux thread plus
separate audio and video consumers**. Audio is the master clock (`/dev/MrAudio`
rptr/len). Video follows that clock through the proven mailbox A/B path.

Single-thread `player/tools/dvd_av_test.c` is a **failed proof**: MPEG-2 decode
starved the only loop that could feed MrAudio (fill min/avg/max 0 / 3.6 / 146 ms;
30 s media took ~146 s wall). Do not patch that scheduler.

`player/tools/dvd_av_threaded_test.c` is the **known-good** combined path. SS1,
title 2 / chapter 1, ~30 s:

| Measurement | Result |
|---|---|
| Visual | Continuous, normal speed, well synchronized |
| Hardware audio consumed | 30.872 s |
| MrAudio underruns | 0 |
| MrAudio fill average | 139.2 ms (target ~150 ms) |
| Video frames displayed | 771 |
| Average video−audio offset | −30.9 ms |
| Frames >80 ms late | 1 |

Do **not** retune queues, A/V clocks, decode, swscale, or mailbox scheduling
from this baseline.

### Known / non-blocking characteristics (not bugs)

- **−30.9 ms mean offset** is mailbox-VBL presentation versus the audio master
  clock with no frame dropping. Lip-sync was judged good on hardware.
- **139.2 ms average fill** versus the 150 ms target is healthy, not starvation.
- **One late frame in 771** is acceptable for this proof.
- **MPEG-PS video is ~340 compressed packets/s**, not one packet per displayed
  frame. `VIDEO_Q_CAP` 384 ≈ 1.1 s. Capacity 64 was ~0.19 s and head-of-line
  stalled demux for essentially the whole run (periodic whole-player pauses).
- Audio queue remains 32 packets (~1 s of AC-3). Backpressure still waits when
  a queue is genuinely full; packets are not dropped.

libdvdnav and the custom AVIO callback run **only on the demux thread**.
Decoder contexts are not shared across threads.
