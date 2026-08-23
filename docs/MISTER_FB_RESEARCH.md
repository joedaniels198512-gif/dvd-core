# MiSTer Framebuffer (MISTER_FB / ASCAL) Research

**Goal:** determine the correct way to get native-resolution DVD frames
(720×576 PAL / 720×480 NTSC) from our ARM player into MiSTer's FPGA
video/scaler path, with the FPGA doing all scaling.

**Verdict up front: the existing MiSTer framework is sufficient. No custom
scaler RTL is needed for HDMI.** There are *two* framebuffer control paths, and
we can start with zero FPGA changes.

---

## 0. Sources inspected (all primary)

| Source | Version inspected |
|---|---|
| `fpga/` (this repo) | Template_MiSTer @ `df59d675a1e11a2390fb0f6ba577909977380b7b` (flattened) |
| [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) | `master` snapshot, 2026-08-23 |
| [Linux-Kernel_MiSTer](https://github.com/MiSTer-devel/Linux-Kernel_MiSTer) | `master`, files fetched 2026-08-23: `drivers/video/fbdev/MiSTer_fb.c`, `sound/drivers/MiSTer-audio-spi.c`, `arch/arm/boot/dts/socfpga_cyclone5_de10_nano.dts`, `arch/arm/configs/MiSTer_defconfig` |
| [ao486_MiSTer](https://github.com/MiSTer-devel/ao486_MiSTer) | `master` `ao486.qsf` (production `MISTER_FB` user, existence proof) |

Line numbers for upstream files are from the master snapshots above and may
drift; local `fpga/` line numbers are exact for our pinned baseline.

---

## 1. MISTER_FB — what it actually is

### 1.1 There are TWO framebuffer control paths, one scaler

The scaler (`ascal`) has a single "framebuffer mode": when `o_fb_ena=1` it
stops displaying captured core video and instead **reads pixels directly from
DDR** at `o_fb_base` (`fpga/sys/ascal.vhd` lines 164–170, 2009–2016). Two
sources can drive those parameters (`fpga/sys/sys_top.v` lines 833–868):

```845:868:fpga/sys/sys_top.v
reg        FB_EN     = 0;
reg  [5:0] FB_FMT    = 0;
reg [11:0] FB_WIDTH  = 0;
reg [11:0] FB_HEIGHT = 0;
reg [31:0] FB_BASE   = 0;
reg [13:0] FB_STRIDE = 0;

always @(posedge clk_sys) begin
	FB_EN <= LFB_EN | fb_en;
	if(LFB_EN) begin
		FB_FMT    <= LFB_FMT;
		FB_WIDTH  <= LFB_WIDTH;
		FB_HEIGHT <= LFB_HEIGHT;
		FB_BASE   <= LFB_BASE;
		FB_STRIDE <= LFB_STRIDE;
	end
	else begin
		FB_FMT    <= fb_fmt;
		FB_WIDTH  <= fb_width;
		FB_HEIGHT <= fb_height;
		FB_BASE   <= fb_base;
		FB_STRIDE <= fb_stride;
	end
end
```

- **`LFB_*` — the HPS/Linux framebuffer.** Programmed by Main_MiSTer over SPI
  command `0x2F` (`UIO_SET_FBUF`, `Main_MiSTer/user_io.h:57`); register writes
  decoded in `sys_top.v` lines 451–464 (enable+format, base lo/hi, width,
  height, HMIN, HMAX, VMIN, VMAX, stride). **This path is compiled into every
  core** — it is *not* gated by `MISTER_FB`, and `sys_top.v:404` ACKs the
  command unconditionally (`if(io_din[7:0] == 'h2F) io_dout_sys <= 1;`). Any
  core built on the current framework supports it, including our stock
  template build.
- **`fb_*` — the core-driven framebuffer, gated by `` `ifdef MISTER_FB ``.**
  Enabling the macro adds `FB_EN/FB_FORMAT/FB_WIDTH/FB_HEIGHT/FB_BASE/
  FB_STRIDE` outputs and `FB_VBL/FB_LL` inputs to the `emu` module
  (`fpga/sys/emu_ports.vh` lines 40–67; wired at `sys_top.v` 1790–1807). The
  *core logic itself* decides what to display, per frame. `LFB_EN` (HPS) wins
  if both are enabled.

Enabling `MISTER_FB` is a one-line change in the **core's own** `.qsf`, not in
`sys/`:

```53:56:fpga/Template.qsf
#set_global_assignment -name VERILOG_MACRO "MISTER_FB=1"
#set_global_assignment -name VERILOG_MACRO "MISTER_FB_ATTENTION=Read instructions before enable it!"
##################################################
#set_global_assignment -name VERILOG_MACRO "MISTER_FB_PALETTE=1"
```

ao486 ships with `MISTER_FB=1` + `MISTER_FB_PALETTE=1` in production
(`ao486_MiSTer/ao486.qsf`).

### 1.2 What else changes when a framebuffer is active

- Scanlines are forced off while `FB_EN` (`sys_top.v:364`).
- With `LFB_EN`, the output window comes from `LFB_HMIN/HMAX/VMIN/VMAX`
  verbatim (`sys_top.v:938–943`); otherwise (including core-driven
  `MISTER_FB`) it is computed from the core's `VIDEO_ARX/ARY` aspect ratio
  (`sys_top.v:902–1010`).
- `FB_FORCE_BLANK` lets the core blank output while `LFB_EN=0`
  (`sys_top.v:1142–1151`).
- `FB_VBL` given to the core is the HDMI vertical blank (`sys_top.v:870–873`).
- In 8bpp palette mode the palette is fetched from DDR at `LFB_BASE − 4096`
  (`sys_top.v:1030–1036`) — this is why the HPS framebuffer lives at a
  `+4096` offset (see §2).

### 1.3 Where the pixels physically live

In the **HPS DDR3 (the same SDRAM Linux uses)**, in the FPGA-reserved region
above the Linux memory limit. ascal fetches them through its Avalon master
(`sys_top.v` 820–830) into the f2sdram bridge (`sysmem.sv`). The relevant map
(DE10-nano / stock MiSTer — assumed identical on SS1, needs one-time
verification):

| Region | Use | Source |
|---|---|---|
| `0x20000000` + 24 MB | ascal capture buffers (RAMBASE, 8 MB × 3 for triple buffering; 2 MB with `MISTER_SMALL_VBUF`) | `sys_top.v:716–721`, `ascal.vhd:96–98` |
| `0x22000000` (= base + 32 MB) | **HPS framebuffer** (`FB_ADDR`), image at `+4096`, palette below | `Main_MiSTer/video.cpp:36–37`, DTS `MiSTer_fb reg = <0x22000000 0x800000>` |
| `0x22000000` + n·(1920·1080·4) | Main's additional menu/wallpaper buffers (n = 1, 2) | `video.cpp:2414`, `3491` |

---

## 2. HPS / Main_MiSTer side

### 2.1 How Main configures the framebuffer

`Main_MiSTer/video.cpp: video_fb_enable()` (≈3474–3543) sends the `0x2F`
sequence: `FB_EN | format`, base lo/hi, width, height, hmin, hmax, vmin, vmax,
stride. Format flags (`video.cpp:46–52`):

```c
#define FB_FMT_565  0b00100
#define FB_FMT_1555 0b01100
#define FB_FMT_888  0b00101
#define FB_FMT_8888 0b00110
#define FB_FMT_PAL8 0b00011
#define FB_FMT_RxB  0b10000   // swap R/B: this + 8888 == our BGR0/XRGB8888
#define FB_EN       0x8000
```

Main uses `FB_EN | FB_FMT_RxB | FB_FMT_8888` for the Linux console — **exactly
the 32-bit layout our player already produces** (X8R8G8B8 little-endian,
R offset 16 / G offset 8 / B offset 0, verified against our framebuffer probe
in `player/src/dvdplayer_headless.c:315–318`).

### 2.2 `/dev/fb0` on MiSTer IS the scaler framebuffer

This was the key "don't assume" item, resolved: MiSTer's `/dev/fb0` is **not**
a separate display device. It is provided by the `MiSTer_fb` kernel driver
(`Linux-Kernel_MiSTer/drivers/video/fbdev/MiSTer_fb.c`,
`CONFIG_FB_MISTER=y`), whose memory is the fixed physical window
`0x22000000 + 4096` (DTS node `MiSTer_fb { reg = <0x22000000 0x800000> }`;
driver maps it `MEMREMAP_WT`, `MiSTer_fb.c:257–266`). So our previous
`/dev/fb0` experiment was *already* writing into the ASCAL path — but with
geometry chosen by Main (a scaled-down copy of the current output mode, see
`video_fb_config()`, `video.cpp:3556–3584`), which is why letting Main pick
the size implies CPU scaling. The fix is to set the framebuffer to **our**
native size, not to abandon `/dev/fb0`.

### 2.3 Userspace interfaces available to our player (no Main modifications)

Confirmed interfaces, all usable by a normal Linux process:

1. **`/dev/MiSTer_cmd`** (FIFO read by Main; `input.cpp:4051`, `6236`):
   - `fb_cmd1 <fmt> <rb> <width> <height>` → switches the scaler to HPS
     framebuffer mode at **arbitrary size** (e.g. `fb_cmd1 8888 1 720 576`),
     base fixed at `FB_ADDR+4096`, stride `(w·bpp + 15) & ~15`, output
     rectangle = largest *integer* multiple centered on screen
     (`video.cpp:4179–4316`). Also reprograms the `MiSTer_fb` kernel module so
     `/dev/fb0` re-registers with the new geometry (`video.cpp:4304–4309`).
   - `fb_cmd0/fb_cmd2 <fmt> <rb> <div>` → full-screen stretch of a
     1/div-size framebuffer.
   - `load_core <rbf>`, `video_mode <modeline>`, `screenshot` also available.
2. **`/dev/fb0`** — mmap the pixels (after `fb_cmd1` it is exactly our
   720×576 BGR0 buffer).
3. **`FBIO_WAITFORVSYNC` ioctl on `/dev/fb0`** — real hardware vsync: FPGA
   raises f2h IRQ 40 on every HDMI vertical sync
   (`sys_top.v:573: f2h_irq = {video_sync, HDMI_TX_VS}`; DTS
   `interrupts = <0 40 1>`; `MiSTer_fb.c:48–54, 96–132`, 50 ms timeout).
   `/sys/module/MiSTer_fb/parameters/frame_count` exposes a vsync counter.
4. **`/sys/module/MiSTer_fb/parameters/mode`** — `fmt rb width height stride`
   read/write (kernel-side geometry only; does not touch the scaler).
5. **`/dev/mem`** — Main itself maps `FB_ADDR` this way (`shmem.cpp:18–27`);
   we can map additional DDR buffers the same way for a future multi-buffer
   scheme.

**Safety:** writing pixels to `/dev/fb0`/`/dev/mem` does not conflict with
Main (Main only draws to it in the menu core). The **SPI link to the FPGA is
owned exclusively by Main** — we must never program `UIO_SET_FBUF` ourselves;
all scaler control goes through `/dev/MiSTer_cmd` (or, later, through our own
core's `MISTER_FB` outputs, which bypass SPI entirely).

---

## 3. ASCAL / video path

Pixel flow in framebuffer mode:

```
DDR3 (HPS SDRAM) ──avalon/f2sdram──▶ ascal line buffers ──▶ interpolators
      ▲                               (o_fb_base, o_fb_stride)      │
      │                                                             ▼
 ARM writes BGR0                        HDMI (and optionally VGA via vga_scaler)
```

- **Formats** (`ascal.vhd:41–45`): 8bpp palette, 16bpp 565/1555, 24bpp,
  32bpp; bit 4 selects RGB/BGR. 32-bit BGR0 (`FB_FMT_RxB|FB_FMT_8888` =
  `6'b010110`) is fully supported and is what MiSTer itself uses for the
  Linux console.
- **Format choice:** BGR0 is the right starting point — zero changes to our
  proven `swscale` path, and DDR bandwidth is trivial (720×576×4×25 fps ≈
  41 MB/s write + similar read, against a multi-GB/s DDR3). RGB565 would
  halve the copy cost (~7 ms → ~3.5 ms/frame at measured rates) at the price
  of banding on DVD gradients; keep it as a fallback, not the default.
  A YUV format would be ideal (skip the 3.5 ms conversion entirely) but
  **ascal has no YUV framebuffer support** — adding it would be custom RTL,
  explicitly not justified yet.
- **Scaling quality:** Nearest/Bilinear/Sharp-Bilinear/Bicubic/Polyphase
  (`ascal.vhd:78–86`), selected by the user's normal MiSTer scaler settings —
  we inherit all of it, including gamma and shadow masks.
- **Downscaling is disabled in framebuffer mode** (`o_hdown/o_vdown` forced 0,
  `ascal.vhd:2014–2015`). 720×576 → 1280×720/1920×1080 is upscaling in both
  axes, so HDMI is fine; but outputs *smaller* than 720×576 (e.g. 15 kHz
  progressive 288p) must not be assumed to work. ⚠️ untested assumption.
- **Aspect ratio/output placement is entirely framework-handled** — from
  `LFB_HMIN..VMAX` (HPS path) or from `VIDEO_ARX/ARY` (core path). Nothing
  needs to scale on ARM.
- **Stride:** explicit `FB_STRIDE` must be a multiple of the pixel size; 0
  means "round line to 256-byte bursts" (`emu_ports.vh:47`,
  `ascal.vhd:2029–2034`). For 720×32bpp: 2880 bytes (what `fb_cmd1` uses) or
  2944 when rounded.

---

## 4. Framebuffer geometry for DVD

MiSTer wants the **display aspect ratio** (DAR), not pixel counts —
`VIDEO_ARX/ARY` (or the LFB window) define the shape of the output rectangle,
so non-square DVD pixels are handled by construction:

| Content | Buffer | DAR to communicate |
|---|---|---|
| PAL 4:3 | 720×576, stride 2880 | 4:3 |
| PAL 16:9 anamorphic | 720×576, stride 2880 | 16:9 |
| NTSC 4:3 | 720×480, stride 2880 | 4:3 |
| NTSC 16:9 anamorphic | 720×480, stride 2880 | 16:9 |

The DAR comes straight from the MPEG-2 sequence header (FFmpeg exposes it as
`sample_aspect_ratio` × dimensions; our player already computes exactly this
in `calculate_output_rect()`, `player/src/dvdplayer_headless.c:503–562`).

Where it is communicated depends on the path:

- **Own core with `MISTER_FB` (end state):** core drives `VIDEO_ARX/ARY`
  (e.g. from a mailbox value the ARM writes) → framework computes the output
  rectangle (`sys_top.v:902–1010`). Exact, dynamic, per-title switchable.
- **HPS `fb_cmd1` path (PoC):** the output rectangle is auto-computed as an
  *integer* scale (`video.cpp:4212–4235`), e.g. 720×576 shown 1:1 inside
  1280×720 — a ~7% AR error (square-pixel display of non-square content).
  Acceptable for a proof of concept, not for the shipping player.
- Optional refinement: crop to the ITU active area (704×576/704×480) before
  display for a strictly correct 4:3; standard players differ here, decide
  later.

---

## 5. Buffering, tearing, synchronization

Confirmed mechanics:

- **ascal latches `o_fb_base` on the output VS falling edge**
  (`ascal.vhd:1732–1736`), so a base-address change is an atomic, tear-free
  page flip at output-frame granularity.
- ascal's own triple buffering (`MODE[3]`, `ascal.vhd:89–90, 1937–2007`)
  applies only to the *core-video capture* path — in framebuffer mode
  multi-buffering is the producer's job (flip `FB_BASE` between frames).
- **Core-driven `MISTER_FB` path:** the core can flip `FB_BASE` freely and
  sees `FB_VBL` (HDMI vblank). This gives clean double/triple buffering.
  25 fps PAL on a 50 Hz output (`720p50`/`1080p50` are standard modes,
  `video.cpp:127–141`) is an exact 2:2 cadence; 29.97 on 59.94 Hz likewise.
  23.976 film NTSC needs 2:3 pulldown or a matching output rate — defer.
- **HPS `fb_cmd` path:** the base is fixed at `FB_ADDR+4096`
  (`video.cpp:4282`) — effectively **single-buffered**. Options within it:
  write during the ~14.7 ms copy while racing the output sweep (~16.7 ms at
  60 Hz) after `FBIO_WAITFORVSYNC` — marginal, may tear; fine for a static
  PoC image, not for the player.
- **Vsync for the ARM:** `FBIO_WAITFORVSYNC` on `/dev/fb0` (hardware IRQ 40 =
  `HDMI_TX_VS`). This is the pacing primitive for playback (replaces our
  `av_usleep` scheduling).

**Conclusion:** continuous tear-free playback ⇒ use the core-driven
`MISTER_FB` path with ≥2 buffers and a small ARM→core mailbox in DDR (§8.3).

---

## 6. CRT / Direct Video

Confirmed from source:

- The scaler output modes in Main are **all progressive** (`vmodes[]`,
  `video.cpp:125–142`). There are 15 kHz **progressive** TV modes (`tvmodes[]`,
  `video.cpp:145–151`: 640×240p NTSC, 640×288p PAL) used with
  `direct_video` — but nothing interlaced, and ascal has no interlaced
  *output* timing generator (its interlace support is input-side:
  `ascal.vhd:14, 27–30`).
- The HPS framebuffer is compatible with `direct_video` — Main offsets the
  window into the TV blanking (`FB_DV_LBRD/UBRD`, `video.cpp:54–57,
  3494–3499`) and routes the framebuffer to VGA (`set_vga_fb`,
  `user_io.cpp:3018–3027`; `vga_fb` muxing `sys_top.v:292–320, 1317–1346`).
- Therefore, through existing infrastructure a CRT can get:
  1. **31 kHz progressive** scaler output via `vga_scaler` (fine for
     tri-sync/VGA CRTs) — high confidence, standard MiSTer feature.
  2. **15 kHz progressive** 240p/288p — but this is a *downscale* of our
     576/480-line buffer, and framebuffer mode disables downscaling
     (§3) — ⚠️ likely broken/half-resolution; hardware test required.
- **True 576i50 / 480i59.94 interlaced CRT output is NOT provided by the
  framebuffer/scaler path.** For authentic interlaced 15 kHz output our core
  will eventually need its own small video timing generator reading DDR
  directly (classic console-core pattern, using `direct_video`/VGA bypass) —
  this is *display timing* RTL, not scaler RTL, and it is a Phase-3 concern.
  HDMI-first is the right sequencing.

Assumption to verify on SS1: how the SS1's analog output board maps onto
MiSTer's `direct_video`/`vga_scaler` plumbing.

---

## 7. Audio (secondary)

The standard framework audio path for Linux software is real and
hardware-paced:

- **FPGA side:** `fpga/sys/alsa.sv` (instantiated `sys_top.v:1639–1656`)
  reads 16-bit stereo PCM from a DDR **ring buffer** via the DDR service and
  consumes it at a fixed **48 kHz** (accumulator `acc += 48000` vs
  `CLK_RATE=24.576 MHz`, `alsa.sv` ≈145–153), mixing it into the core's audio
  (HDMI + analog). Buffer address/length/write-pointer arrive over a
  dedicated HPS SPI (`spi0`), and the FPGA reports its **read pointer** back;
  a `hurryup` mechanism speeds consumption if the buffer over-fills.
- **Kernel side:** `sound/drivers/MiSTer-audio-spi.c`
  (`CONFIG_SND_MISTER_AUDIO=y`, DTS `compatible="MiSTer,spi-audio"`) exposes
  **`/dev/MrAudio`**: `write()` copies PCM into a 512 KB CMA ring
  (~2.6 s @ 48 kHz stereo S16) and SPIs the new write pointer;
  `open()+read()` returns a status line `rptr: … wptr: … len: …` with the
  FPGA's live read pointer. Writes do **not** block on buffer fullness — flow
  control means polling `rptr`.
- **There is no ALSA PCM card for this path** (despite living under
  `CONFIG_SND_*`, it is a plain char device). `aplay` to it is not the
  mechanism; direct `/dev/MrAudio` writes are the standard interface — i.e.
  our existing `/dev/MrAudio` experiments were already on the right path. The
  improvement available: pace by polling `rptr` (target ~100–200 ms buffer
  depth) instead of wall-clock timing, and use the FPGA's 48 kHz consumption
  as the master clock for A/V sync (DVD AC-3 is natively 48 kHz — no
  resampling needed).

Not implementing yet, as instructed.

---

## 8. Minimal proof of concept

**Objective:** boot `DVD_Core.rbf` (renamed stock template), have a tiny ARM
program display ONE native 720×576 image via the existing framework scaler on
HDMI. No DVD code involved.

### 8.1 How it works (zero custom RTL)

```
ARM PoC program                          FPGA (unmodified framework)
──────────────────                       ───────────────────────────
echo "fb_cmd1 8888 1 720 576"            Main sends UIO_SET_FBUF(0x2F)
      > /dev/MiSTer_cmd          ──────▶ sys_top LFB_* regs → ascal fb mode
mmap /dev/fb0 (now 720×576 BGR0          ascal reads 0x22001000 via avalon,
  at phys 0x22001000)                    upscales (bilinear/poly), outputs HDMI
ioctl FBIO_WAITFORVSYNC                  IRQ40 = HDMI_TX_VS
write test image once
```

ARM↔FPGA communication: entirely via existing Main_MiSTer (`/dev/MiSTer_cmd`)
and the `MiSTer_fb` kernel driver. Output format: BGR0/XRGB8888 (`8888`,
`rb=1`), 720×576, stride 2880.

### 8.2 Exact changes

Files to **add**:

| File | Purpose |
|---|---|
| `player/tools/fb_poc.c` | ~150-line test: send `fb_cmd1 8888 1 720 576`; mmap `/dev/fb0`; verify `FBIOGET_VSCREENINFO` reports 720×576/32bpp; draw SMPTE-style bars + resolution grid; `FBIO_WAITFORVSYNC` loop to confirm vsync IRQ works (log timing for 500 frames); restore nothing (screen returns on core reload). |
| `player/tools/build_fb_poc.sh` | Cross-compile with `arm-unknown-linux-gnueabihf-gcc` + the proven VFPv3 flags + `objdump` VFPv4 gate (copy pattern from `build_mac.sh`). |

Files to **modify** (rename-only, no functional change):

| File | Change |
|---|---|
| `fpga/Template.qpf` → `DVD.qpf` | `PROJECT_REVISION = "DVD"` |
| `fpga/Template.qsf` → `DVD.qsf` | copy as-is |
| `fpga/Template.sv` → `DVD.sv` | `CONF_STR` `"Template;;"` → `"DVD;;"` |
| `fpga/Template.sdc/.srf` → `DVD.*` | copy as-is |

Nothing in `fpga/sys/` or `fpga/rtl/` changes. **Phase 0 can even skip the
RBF entirely** — `fb_cmd1` + `/dev/fb0` work in the stock menu core, so the
ARM-side path can be validated *today* without any Quartus compile; the
`DVD_Core.rbf` build (slow under Rosetta) can happen once, later, purely to
prove "our own core + HPS framebuffer".

### 8.3 The step after the PoC (for context, not part of it)

Switch to the core-driven path for the real player: uncomment
`MISTER_FB=1` in `DVD.qsf`, add ~100–200 lines in `fpga/rtl/` that read a
16-byte **mailbox** (frame base, ARX/ARY, frame counter) from DDR via the
`DDRAM` port each `FB_VBL`, and drive `FB_BASE/FB_WIDTH/FB_HEIGHT/FB_FORMAT/
FB_STRIDE` + `VIDEO_ARX/ARY` from it. ARM mmaps two 720×576 buffers + mailbox
via `/dev/mem` in reserved DDR, decodes into the back buffer, flips by
updating the mailbox, paced by `FBIO_WAITFORVSYNC`. That yields tear-free
double buffering and exact 4:3/16:9 aspect — still zero scaler RTL.

### 8.4 Assumptions that still need hardware testing

1. SS1 runs stock-enough MiSTer Main + kernel: `/dev/MiSTer_cmd`,
   `MiSTer_fb` module, and IRQ 40 all present (`dmesg | grep -i
   "MiSTer_fb\|MrAudio"`, `ls /dev/MiSTer_cmd`).
2. `fb_cmd1` accepted while a non-menu core is loaded (code path is
   core-agnostic, but untested by us).
3. `FBIO_WAITFORVSYNC` returns at the output rate (not `-ETIMEDOUT`) on SS1.
4. Actual copy bandwidth into the `/dev/fb0` mapping at 720×576 (~1.66 MB;
   prior 14.7 ms measurement was at a different geometry/mapping).
5. SS1 DDR map matches DE10-nano (`0x22000000` window) — implied by our
   earlier successful `/dev/fb0` write, but confirm via
   `cat /proc/device-tree/MiSTer_fb/reg` or dmesg.
6. 50 Hz output modes (`720p50`) acceptable to the user's display for PAL
   cadence.
7. Restoring normal video after `fb_cmd` (menu round-trip or core reload) —
   no explicit "fb off" command exists in `/dev/MiSTer_cmd`.

---

## 8.5 HARDWARE TEST RESULT (2026-08-23) — fb_cmd1 path REJECTED

`player/tools/fb_native_test` was run on the SS1. **`fb_cmd1 8888 1 720 576`
did not resize `/dev/fb0`; it remained 1920×1080.** The HPS `fb_cmd` path is
therefore NOT viable on the SS1 as shipped.

Possible causes (unverified, and no longer worth chasing): the SS1's
Main_MiSTer build may predate or diverge from the upstream `video_cmd()`
behaviour analysed in §2.3, or menu-core framebuffer management
(`video_fb_enable`/`video_fb_config`) may immediately re-assert its own
geometry over the custom mode.

**Consequence:** skip straight to the core-driven `MISTER_FB` path (§8.3).
That path does not involve Main_MiSTer in framebuffer configuration at all —
the core's own `FB_*` outputs program the scaler directly (`sys_top.v`
845–868), so it cannot be affected by Main version differences. The ARM
writes pixels via `/dev/mem` into a fixed DDR address instead of `/dev/fb0`.
`FBIO_WAITFORVSYNC` on `/dev/fb0` remains usable for pacing (the vsync IRQ is
independent of framebuffer geometry).

---

## 9. Summary of conclusions

1. **MISTER_FB/ASCAL is suitable and sufficient** for the HDMI architecture;
   pixel format, sizes, scaling, and aspect handling all match our needs.
2. ~~Two-stage adoption starting with the HPS framebuffer~~ **Superseded by
   §8.5:** the `fb_cmd` HPS path failed on SS1 hardware (no resize). Go
   directly to the core-driven `MISTER_FB` path: constant `FB_*` outputs
   first (static geometry, single buffer), then the DDR mailbox for double
   buffering and dynamic AR — small glue in the core only, no `sys/` changes,
   no scaler RTL.
3. **Custom RTL is NOT needed for HDMI.** The only place custom video RTL is
   foreseeably required is true interlaced 15 kHz CRT output (Phase 3).
4. `/dev/fb0` was never the wrong interface — it *is* the scaler framebuffer;
   the wrong part was letting Main choose its geometry (which forced CPU
   scaling). Setting our own native geometry eliminates the 104 ms/frame
   scaling cost entirely.
5. Audio: `/dev/MrAudio` (48 kHz S16 stereo ring buffer consumed by
   `alsa.sv`) is the standard hardware-paced path; pace via its read-pointer
   readback.
