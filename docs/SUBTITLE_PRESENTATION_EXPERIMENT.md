# Subtitle presentation experiment

This candidate targets choppy movie playback when subtitles are enabled in the
buffered FPGA YUV420 path. It retains the 128-packet audio queue experiment.
Hardware playback validation is pending; a successful cross-build does not prove
that this fixes the reported stutter.

## Revision 3: clear inherited single-CPU affinity

The next device log showed almost-empty decoded-video queues even with subtitles
off, while the publication lock added in revision 2 held for only tens of
microseconds. Inspection of `/proc/<pid>/task/*/status` on the user's SuperStation
found all five standalone player threads restricted to CPU 1. MiSTer_DVD's main
thread was also pinned there, while CPU 0 was online. The player's old
"unpinned" diagnostics only reported its current CPU; they never cleared the
launcher-inherited affinity mask.

A live test changed only the running player's thread affinity from mask 2 to
mask 3 (CPUs 0 and 1). The user reported smoother subtitles and more responsive
menu navigation. Subsequent movie samples showed 23-25/25 queued video frames,
roughly 29-30 fps and no audio underruns in the new test segment. Three additional
stale drops appeared between the later 10- and 20-second reports: this is
encouraging single-device evidence, not proof of hitch-free playback on all DVDs.
The pause-spanning FPS sample and long menu interval are not movie benchmarks.

Revision 3 restores eligibility on configured CPUs before creating workers in
buffered FPGA-YUV playback. The kernel still enforces online/cpuset restrictions.
The launcher and other services are unchanged, and benchmark/other video paths
retain their existing affinity behavior. A failure logs a warning and playback
continues. The startup log reports `SCHED: player CPU eligibility 1 -> 2 CPUs`
when correcting the observed restriction. Already unrestricted launches may
report `2 -> 2`. Worker diagnostics now describe the inherited player mask.

`player/tools/test_player_cpu_affinity.c` exercises the actual helper on Linux.
It passed on the user's SuperStation as a separate ARM process: recovery from
each single-CPU mask, worker-thread inheritance and repeated setup. The test
restored its own initial affinity and left the SSH parent mask unchanged.
The revised full player still needs a restart test and longer playback coverage.
Include `player_cpu_affinity.h` beside the player source when rebuilding.

Reference: [Linux CPU-affinity inheritance](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html).

## Revision 2: remove subtitle decode from the presentation lock

The first candidate's device log still shows severe intermittent stalls. At
raw video PTS 47.8615 seconds, `prepare_before_ack_us=139185`, while scratch copy
and composition take only 1218 and 5942 microseconds. The remaining roughly
132 ms is outside those two measurements, and the stall follows a subtitle
enqueue. Another stale-drop burst reports a previous frame cycle of 357595 us
with only 3 us ACK wait and 11938 us final display copy. Video remains queued.
This strongly implicates blocking in subtitle preparation; the first candidate
did not measure mutex waiting directly and cannot prove the exact breakdown.

Source inspection shows the demux thread holding `msub.mu` across RLE decoding,
full authored-bitmap analysis, cropping, run construction and diagnostic output.
Both subtitle visibility checks and composition on the presentation thread
need that lock. Revision 2 performs that work in a demux-owned staging plane,
then publishes the completed metadata and swaps bitmap/run buffers during a
short critical section. It adds one reusable plane (about 0.5 MiB), not another
thread. Queue capacity and the first candidate's blending/presentation changes
remain unchanged. Navigation generations and a subtitle-reset epoch reject
obsolete staged work. The current displayed subtitle is never recycled by
the publisher. A full queue retains the existing drop behavior.

`SUBTITLE DECODE` now records decode/analysis time, publication lock wait, and
publication lock hold time. Timeline and enqueue output run outside that lock.
Compare these with `SUBTITLE PRESENT` and stale drops in the next device log.
The enqueue log's `aclk_at_decode` field now samples the clock at publication,
after decoding, rather than before bitmap decoding.

Run `python3 player/tools/test_subtitle_publish.py` for native tests of the
actual production publication helper, with a minimal player/clock harness.
Address/undefined-behavior sanitizers cover buffer ownership, full and expired
queues, reset/navigation invalidation, a concurrent presenter holding the lock,
and 1000 storage reuse cycles. These are queue-publication tests, not hardware
playback or full subtitle-decoder tests.

## Evidence and proposed change

In the supplied on/off log, one approximately 21-second subtitles-off interval
had one stale-frame drop; the next approximately three-second on interval had
14; the following approximately 93-second off interval had five. During choppy
playback the decoded-video queue remained populated, unlike the original audio
queue starvation. This supports investigating subtitle presentation overhead,
but does not by itself isolate its exact cost.

Previously the presenter waited for the FPGA to acknowledge the preceding
display request, then copied the next video frame to cached scratch RAM and
composed its subtitles, then copied the result to the free display buffer.
This candidate performs the scratch preparation before that wait for timed
movie frames, allowing it to overlap the outstanding display request. It still
waits for the ACK and reads the actual display-buffer status before choosing or
writing the destination. Decoder frames remain unmodified. Menu, UI redraw,
and still-frame paths retain preparation after the wait. Navigation generation
checks discard obsolete prepared frames; an on/off change during the wait
causes preparation to be repeated using the current setting.

Uniform movie subtitle palettes also use a small cache: four luma blend tables
and 256 alpha-weighted chroma combinations for the four 2-bit codes in a 2x2
block. Palette or alpha changes rebuild the cache. Changing video backgrounds
are blended afresh on every frame. Clipped/partial chroma blocks and spatial
CHG_COLCON palette changes retain the original scalar calculation. Integer
rounding and authored Y/Cr/Cb values are preserved.

## Validation performed locally

Run the native differential test from the repository root:

```sh
cc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  player/tools/test_subtitle_blend_cache.c -o /tmp/test-subtitle-cache
/tmp/test-subtitle-cache
```

It compares the cached blend against the previous scalar arithmetic for 210
palette/alpha states, all 256 pixel-code blocks and all 256 background values
in each plane (13,762,560 block/background comparisons), plus luma, cache
invalidation, odd bitmap stride, and edge fallback checks. It does not exercise
DVD navigation, full SPU decoding, or FPGA timing.

The standalone player is cross-built with the existing Cortex-A9 ARM hard-float,
NEON/VFPv3 flags and static FFmpeg libraries. The test package records its ELF,
library/symbol-version checks and checksums. No RBF or shared-library changes
are required. Include `subtitle_blend_cache.h` beside the player source when
rebuilding from a source bundle.

## MiSTer comparison

Exit playback and back up `/media/fat/DVD/bin/dvd_av_threaded_test`. Replace it
with the candidate binary and keep the existing libraries, core and settings.
The package includes the previous 128-packet player as the control/rollback;
the 32-packet player would reintroduce the earlier audio-queue variable.

Replay the same subtitle-heavy chapter in both builds, ideally for at least
60 seconds each with subtitles on, then repeat the on/off toggle. Check subtitle
colors, outlines and fades, audio sync, chapter jumps, pause/resume, and menu
highlights. Save each complete `player.log` separately, recording the hardware,
disc/ISO, PAL/NTSC and installed binary checksum. PAL, NTSC, physical media,
ISO, and authored spatial palette changes need hardware coverage.

Compare rendered FPS, stale drops, queue depths and audio underruns. The new
`SUBTITLE PRESENT` samples report total preparation before ACK, scratch-copy,
compose, final display-copy and ACK-wait times. Total preparation includes
visibility checks, locking, allocation and cache setup, which the narrower
compose measurement omits. `yuv_copy_us` remains the final DDR copy only;
use total frame/mailbox cycle and stale drops to judge improvement. Observed
mailbox-to-ACK time may now include preparation before the ACK is polled and
must not be read as a pure hardware latency measurement.
