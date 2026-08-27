/*
 * dvd_av_threaded_test.c — threaded A/V player (direct-DDR + stale recovery).
 *
 * SS1, title 2 / chapter 1. A/B presentation uses FPGA display_buf ack
 * (0x30400008 bit 31) for buffer ownership only, not HDMI FBIO_WAITFORVSYNC
 * and not a synthetic native-frame grid. Convert YUV→BGR0 with sws_scale
 * directly into the inactive DDR buffer after the previous display_buf ack;
 * PTS-hold against the MrAudio consumed-samples clock (+2 ms); mailbox;
 * return immediately.
 *
 * If a decoded frame is already stale versus the audio clock (the following
 * source frame would already be due), discard it before ACK/sws/DDR/mailbox
 * so a missed native PAL period does not accumulate 40 ms of permanent debt.
 * Packets are not dropped; only already-decoded late pictures are skipped.
 * Audio is never altered to follow video. Threads stay unpinned.
 *
 * Playback continues until title end, dvdnav stop, fatal error, or Ctrl+C.
 *
 * Known characteristics from the original combined proof (tag
 * working-threaded-av, no stale recovery):
 *   30.872 s audio consumed, 0 MrAudio underruns, avg fill 139.2 ms
 *   771 frames displayed, avg video−audio offset −30.9 ms
 *   MPEG-PS yields ~340 compressed video packets/s, not 1 packet/frame.
 *     VIDEO_Q_CAP 384 ≈ 1.1 s; 64 was ~0.19 s and HOL-stalled demux.
 *   Single-thread dvd_av_test.c is the failed contrast (fill 0/3.6/146 ms,
 *     30 s media in ~146 s wall). Do not patch that scheduler.
 *
 * Threads (FFmpeg contexts are not shared):
 *   demux  — av_read_frame + libdvdnav/AVIO only; bounded packet queues
 *   audio  — AC-3 + swr + MrAudio; publishes hardware audio clock
 *   video  — MPEG-2 parser/decode + swscale + mailbox; follows audio clock
 *
 * Queues: audio 32 pkts (~1 s AC-3), video 384 pkts (~1.1 s). Backpressure
 * waits when a queue is genuinely full; compressed packets are not dropped.
 *
 * libdvdnav/custom AVIO run only on the demux thread (the AVIO callback is
 * invoked from av_read_frame). Opening the DVD from two threads would be
 * unsafe; that is not done here.
 */

#define _GNU_SOURCE

#include <dvdnav/dvdnav.h>
#include <dvdnav/dvdnav_events.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/cpu.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fb.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define DVD_SECTOR      2048
#define AVIO_BUF_SIZE   (64 * 1024)

#define OUT_RATE        48000
#define OUT_CHANNELS    2
#define OUT_BYTES       4
#define BYTES_PER_SEC   (OUT_RATE * OUT_BYTES)

#define MRAUDIO_DEV     "/dev/MrAudio"
#define WRITE_CHUNK     4096
#define TARGET_FILL     (BYTES_PER_SEC * 150 / 1000)  /* 28800 bytes      */
#define READY_FILL      (BYTES_PER_SEC * 100 / 1000)  /* 19200 bytes      */
#define PAUSE_DRAIN_THRESH 8  /* one alsa.sv 64-bit fetch; 2 samples = 41.7 µs */
#define PAUSE_STUCK_US     500000
#define PRIME_BYTES     256

enum {
    THR_RUN = 0,
    THR_PAUSE,
    THR_QFULL,
    THR_QEMPTY,
    THR_PREFILL,
    THR_CLOCK
};

#define AUDIO_Q_CAP     32
#define VIDEO_Q_CAP     384
#define VIDEO_BUFFER_FRAMES   25
#define VIDEO_PREFILL_FRAMES  12
#define PHASE_YUV_LOW_WATER   10  /* decode anyway if YUV queue this low */
#define PHASE_VQ_HIGH_WATER   (VIDEO_Q_CAP / 2)  /* drain vq so demux/audio live */
#define PCM_HOLD_MAX    (BYTES_PER_SEC * 8)
/* First ~150 ms of decoded PCM used to prime MrAudio after video prefill.
 * Extra PCM decoded during prefill is discarded so the consumed-samples
 * clock does not jump seconds ahead of queued video. */
#define PCM_HOLD_START_BYTES  TARGET_FILL

#define FB_A_PHYS       0x30000000UL
#define FB_B_PHYS       0x30200000UL
#define MB_PHYS         0x30400000UL
#define JOY_OFF         8
#define SET_OFF         16           /* FPGA→ARM DVD-v1 settings (DVD2) */
#define CTL_OFF         24           /* ARM→FPGA source-standard (DVD3) */
#define JOY_MAGIC       0x44564431u  /* "DVD1" at 0x3040000C */
#define SET_MAGIC       0x44564432u  /* "DVD2" at 0x30400014 */
#define CTL_MAGIC       0x44564433u  /* "DVD3" at 0x3040001C */
#define SET_VER         1
#define SET_YUV_CAP_BIT 2            /* DVD2 pad: FPGA YUV420 reader present */
#define MB_YUV_BIT      1            /* mailbox 0x30400000: 1 = planar YUV420 */
#define YUV_Y_OFF       0x000000u
#define YUV_U_OFF       0x080000u
#define YUV_V_OFF       0x0A0000u
#define YUV_Y_STRIDE    720
#define YUV_C_STRIDE    360
#define FPGA_SRC_UNKNOWN 0
#define FPGA_SRC_NTSC    1
#define FPGA_SRC_PAL     2
#define DISP_BUF_BIT    31           /* display_buf in joystick word */
/* joystick_0[30:0] published at 0x30400008; bit 31 is display_buf, not a button.
 * CONF_STR J1 in fpga/DVD.sv: D-pad implicit 0-3, named buttons 4-9.
 * Physical EAST (SNES A) is NOT a spare mailbox bit. Bit 4 is J1[0]; the
 * J1 name "Select" binds SYS_BTN_SELECT (Minus) unless FPGA J1 is renamed. */
#define JOY_BTN_MASK        0x3FFu
#define JOY_BIT_RIGHT       0
#define JOY_BIT_LEFT        1
#define JOY_BIT_DOWN        2
#define JOY_BIT_UP          3
#define JOY_BIT_SELECT      4  /* J1[0] CONFIRM; EAST only after FPGA remap */
#define JOY_BIT_BACK        5  /* J1[1] CANCEL (jn/jp B) */
#define JOY_BIT_PLAYPAUSE   6
#define JOY_BIT_MENU        7
#define JOY_BIT_PREV        8
#define JOY_BIT_NEXT        9
/* Bits 0-9 are all assigned. No spare ARM-visible single button exists;
 * AUDIO NEXT is PREV+NEXT chord until a later FPGA mapping. */
#define CTRL_POLL_US        8000
#define CTRL_REPEAT_DELAY_US  400000
#define CTRL_REPEAT_RATE_US   120000
#define CANCEL_EXIT_HOLD_US   3000000  /* hold B/CANCEL to return to launcher */
#define NAVQ_CAP            16
#define HL_BORDER_PX        4
/* libdvdnav 4.x: btn_coli[][select:0 / action:1] */
#define HL_MODE_SELECT      0
#define HL_MODE_ACTION      1
#define SPU_MAX_PKT         0x10000
#define MSUB_EVT_MAX        32
#define MSUB_CHG_MAX        8
#define MSUB_BAND_MAX       16
#define MSUB_PX_MAX         48
#define MSUB_EVT_COLOR      1
#define MSUB_EVT_CONTR      2
#define MSUB_EVT_CHG        3

typedef struct {
    uint16_t start_col;
    uint8_t color[4];
    uint8_t alpha[4];
} MsubPx;

typedef struct {
    uint16_t y0, y1;
    uint8_t n_px;
    uint8_t px0;
} MsubBand;

typedef struct {
    uint8_t n_bands;
    uint8_t n_px;
    MsubBand bands[MSUB_BAND_MAX];
    MsubPx px[MSUB_PX_MAX];
} MsubChg;

typedef struct {
    int64_t time_us;
    uint8_t kind;
    uint8_t chg_i;
    uint8_t color[4];
    uint8_t alpha[4];
} MsubEvt;
#define NAV_WAIT_FIFO_US    400000
#define STILL_DRAIN_WAIT_US 750000
#define VQ_MARK_STILL_BOUNDARY ((void *)(uintptr_t)0x53544c42u) /* STLB */
#define ACK_TIMEOUT_US       200000   /* warning; keep waiting for ownership */
#define ACK_HARD_TIMEOUT_US  2000000  /* stuck FPGA / forced Buffer B / etc */
#define ACK_REISSUE_MIN_US   25000
#define ACK_REISSUE_MAX_US   40000
#define PRESENT_PERF_INTERVAL 25      /* ~1/s PAL; successful presents only */
#define ISO_SAMPLE_CAP           4096
#define ISO_WARM_PRESENTS        2
#define SWS_DDR_ITERS            1000
#define SWS_DDR_WARMUP           8
#define FB_W            720
#define FB_H_PAL        576
#define FB_H_NTSC       480
#define FB_H            FB_H_PAL   /* DDR mmap / allocation height */
#define FB_STRIDE       2880
#define FB_SIZE         ((size_t)FB_STRIDE * FB_H)
#define SPU_IDX_MAX     (FB_W * FB_H)
#define MB_MAP_SIZE     4096

#define STARTUP_DISPLAY_VBLS     2
#define MAILBOX_POLL_MARGIN_US   2000
#define VSYNC_SAMPLE_INTERVALS   30
#define DISC_JUMP_US             80000
/* Hold until vpts <= aclk + 2 ms, then mailbox. Known-good baseline. */
#define EARLY_SLACK_US           2000
#define MAX_HOLD_US              500000
#define OFFSET_LOG_N             8
#define MISS_LOG_CAP             64

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

static volatile sig_atomic_t stage = 0;
static volatile sig_atomic_t g_interrupt = 0;
static int g_debug_stats = 0;
static int g_debug_spu = 0;
static int g_debug_subtitles = 0;
static int g_debug_yellow_highlight = 0;

static void dbg(const char *fmt, ...)
{
    va_list ap;

    if (!g_debug_stats)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void spu_dbg(const char *fmt, ...)
{
    va_list ap;

    if (!g_debug_spu)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void sub_dbg(const char *fmt, ...)
{
    va_list ap;

    if (!g_debug_subtitles)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void dvdnav_log_cb(void *priv, dvdnav_logger_level_t level,
                          const char *fmt, va_list ap)
{
    (void)priv;
    if ((level == DVDNAV_LOGGER_LEVEL_INFO ||
         level == DVDNAV_LOGGER_LEVEL_DEBUG) &&
        !g_debug_stats)
        return;
    fprintf(stderr, "dvdnav: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static const dvdnav_logger_cb g_dvdnav_logcb = { .pf_log = dvdnav_log_cb };

static const char *stage_names[] = {
    "startup", "dvdnav", "mpeg demux", "packet read",
    "decoder", "frame decode", "ddr framebuffer", "mraudio/swscale"
};

static void sigill_handler(int sig, siginfo_t *si, void *ctxv)
{
    (void)sig;
    ucontext_t *uc = (ucontext_t *)ctxv;
    unsigned long pc = 0, lr = 0;
#if defined(__arm__)
    pc = (unsigned long)uc->uc_mcontext.arm_pc;
    lr = (unsigned long)uc->uc_mcontext.arm_lr;
#endif
    dprintf(STDERR_FILENO, "\n*** SIGILL / illegal instruction ***\n");
    dprintf(STDERR_FILENO, "Stage %d: %s\n", (int)stage,
            (stage >= 0 &&
             stage < (int)(sizeof(stage_names) / sizeof(stage_names[0])))
                ? stage_names[stage] : "unknown");
    dprintf(STDERR_FILENO, "PC=0x%08lx LR=0x%08lx fault_addr=%p\n",
            pc, lr, si ? si->si_addr : NULL);
    _exit(132);
}

static void install_sigill_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigill_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
}

static void interrupt_handler(int sig)
{
    (void)sig;
    g_interrupt = 1;
}

static void install_interrupt_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = interrupt_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void fferr(int err, char *buf, size_t n)
{
    if (av_strerror(err, buf, n) < 0)
        snprintf(buf, n, "FFmpeg error %d", err);
}

static int64_t tb_to_us(AVRational tb, int64_t ticks)
{
    if (tb.num <= 0 || tb.den <= 0 || ticks == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;
    return av_rescale_q(ticks, tb, (AVRational){1, 1000000});
}

typedef struct Player Player;

typedef enum {
    NAVCMD_NONE = 0,
    NAVCMD_MENU,
    NAVCMD_CANCEL,
    NAVCMD_UP,
    NAVCMD_DOWN,
    NAVCMD_LEFT,
    NAVCMD_RIGHT,
    NAVCMD_ACTIVATE,
    NAVCMD_PREVIOUS_CHAPTER,
    NAVCMD_NEXT_CHAPTER,
    NAVCMD_PLAY_PAUSE,
    NAVCMD_AUDIO_NEXT
} NavCmd;

/* ------------------------------------------------------------------ */
/* DVD -> AVIO  (demux thread only)                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    dvdnav_t *nav;
    uint8_t *sector;
    int sector_len, sector_pos, stopped;
    unsigned long nav_packets, mpeg_sectors;
    Player *player;
    int hop_pending;
    int still_len;          /* 0 = none, 0xff = infinite, else seconds */
    int64_t still_t0_us;
    int still_logged;
    int still_armed;        /* STILL seen; wait deferred until MPEG bytes returned */
    int wait_armed;         /* WAIT seen; wait deferred until MPEG bytes returned */
    pci_t pci;
    int pci_valid;
    unsigned pci_lbn;
    unsigned pci_gen;
    DVDDomain_t domain;
    int32_t title, part;
    int32_t last_title, last_part;
    int cellN, pgN, pgcn, pgn;
    int vtsN;
    int activate_trace;
    DVDDomain_t act_domain;
    int32_t act_title;
    int act_cellN;
    int soft_hop_active;
    int soft_drop_pre_hop;
    int post_soft_bytes;
    int post_soft_block_ok;
    int post_soft_mpeg_ret;
    unsigned flush_n;
    int skip_chapter_log;
    int hl_leave_logged;
    int menu_exiting;
} DVDIO;

static int dvdio_pump(DVDIO *d, int32_t event, int32_t len);
static void dvdio_process_nav_cmds(DVDIO *d);
static void dvdio_handle_still(DVDIO *d, int length);
static void dvdio_handle_wait(DVDIO *d);
static void dvdio_note_mpeg_return(DVDIO *d, int n);
static int player_menu_has_frame(Player *p);
static void player_request_still_drain(Player *p);
static void player_wait_still_drain(Player *p, DVDIO *d);
static void prefill_release(Player *p, const char *reason);
static int prefill_is_released(Player *p);
static void navq_post(Player *p, NavCmd cmd);
static void pause_wake(Player *p);
static void pause_cancel(Player *p);
static int pause_is_held(Player *p);
static int pause_should_hold_audio(Player *p);
static void pause_wait_unheld(Player *p);
static int pause_wait_control(Player *p);
static int navq_count(Player *p);
static void dvdio_leave_menu(DVDIO *d);
static int player_active_h(const Player *p);
static void present_draw_highlight(Player *p, uint8_t *dst, int stride,
                                   int frame_menu);
static void movie_sub_reset(Player *p, const char *why);
static int movie_sub_overlay(Player *p, uint8_t *dst, int stride,
                             int64_t pvpts_us, int64_t *blend_us_out);

static int dvd_read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    DVDIO *d = opaque;
    int out = 0;

    while (out < buf_size) {
        if (d->stopped || g_interrupt) {
            if (out > 0)
                dvdio_note_mpeg_return(d, out);
            return out ? out : AVERROR_EOF;
        }
        if (d->hop_pending) {
            /* Drop any pre-hop sector bytes so MPEG-PS does not mix menu
             * leftover with the title start. Hard domain change only. */
            d->sector_len = 0;
            d->sector_pos = 0;
            return AVERROR_EOF;
        }
        dvdio_process_nav_cmds(d);
        if (d->stopped || g_interrupt) {
            if (out > 0)
                dvdio_note_mpeg_return(d, out);
            return out ? out : AVERROR_EOF;
        }
        if (d->hop_pending) {
            d->sector_len = 0;
            d->sector_pos = 0;
            return AVERROR_EOF;
        }
        if (d->soft_drop_pre_hop) {
            /* Pre-hop leftover in this fill is the old PGC. Drop it, then
             * keep reading so the new VOBU reaches MPEG-PS in this or the
             * next read. Do not EOF/reopen. */
            out = 0;
            d->sector_len = 0;
            d->sector_pos = 0;
            d->soft_drop_pre_hop = 0;
        }
        if (d->sector_pos < d->sector_len) {
            int remain = d->sector_len - d->sector_pos;
            int take = buf_size - out;
            if (take > remain)
                take = remain;
            memcpy(buf + out, d->sector + d->sector_pos, (size_t)take);
            d->sector_pos += take;
            out += take;
            continue;
        }
        if (d->player && pause_is_held(d->player)) {
            if (out > 0) {
                dvdio_note_mpeg_return(d, out);
                return out;
            }
            pause_wait_control(d->player);
            continue;
        }
        /*
         * STILL/WAIT must not block while this read already holds NAV/BLOCK
         * bytes. Blocking here prevents MPEG-PS from ever seeing the still
         * VOBU → 0 decoded frames, frozen last DDR picture, nav still alive.
         */
        if (out > 0 && (d->still_armed || d->wait_armed)) {
            dvdio_note_mpeg_return(d, out);
            return out;
        }
        if (d->wait_armed) {
            d->wait_armed = 0;
            dvdio_handle_wait(d);
            continue;
        }
        if (d->still_armed) {
            int slen = d->still_len ? d->still_len : 0xff;

            d->still_armed = 0;
            dvdio_handle_still(d, slen);
            continue;
        }
        int32_t event = 0, len = 0;
        dvdnav_status_t st =
            dvdnav_get_next_block(d->nav, d->sector, &event, &len);
        if (st != DVDNAV_STATUS_OK) {
            fprintf(stderr, "dvdnav error: %s\n",
                    dvdnav_err_to_string(d->nav));
            d->stopped = 1;
            if (out > 0)
                dvdio_note_mpeg_return(d, out);
            return out ? out : AVERROR_EOF;
        }
        switch (event) {
            case DVDNAV_NAV_PACKET:
            case DVDNAV_BLOCK_OK:
            case DVDNAV_WAIT:
            case DVDNAV_STILL_FRAME:
            case DVDNAV_VTS_CHANGE:
            case DVDNAV_HOP_CHANNEL:
            case DVDNAV_CELL_CHANGE:
            case DVDNAV_HIGHLIGHT:
            case DVDNAV_SPU_STREAM_CHANGE:
            case DVDNAV_SPU_CLUT_CHANGE:
            case DVDNAV_AUDIO_STREAM_CHANGE:
            case DVDNAV_STOP:
            case DVDNAV_NOP:
                if (dvdio_pump(d, event, len) < 0) {
                    if (out > 0)
                        dvdio_note_mpeg_return(d, out);
                    return out ? out : AVERROR_EOF;
                }
                continue;
            default:
                continue;
        }
    }
    if (out > 0)
        dvdio_note_mpeg_return(d, out);
    return out;
}

typedef struct {
    const char *device;
    int title, chapter, list_titles;
    int initial_video_skip;
    int video_advance_ms;
    int uncapped_video_benchmark;
    int buffered_video;
    int buffered_yuv;
    int perf_present_no_convert;
    int perf_sws_ddr;
    int phase_decode;
    int debug_stats;
    int debug_spu;
    int debug_subtitles;
    int debug_yellow_highlight;
    int authored_start;
    int fpga_yuv420;
} Cli;

static void usage(void)
{
    fprintf(stderr,
            "Usage: dvd_av_threaded_test [device] [--title N] [--chapter N]\n"
            "                              [--initial-video-skip N]\n"
            "                              [--video-advance-ms N]\n"
            "                              [--uncapped-video-benchmark]\n"
            "                              [--buffered-video]\n"
            "                              [--buffered-yuv-video]\n"
            "                              [--perf-present-no-convert]\n"
            "                              [--perf-sws-ddr]\n"
            "                              [--buffered-yuv-phase-decode]\n"
            "                              [--fpga-yuv420]\n"
            "                              [--debug-stats]\n"
            "                              [--debug-spu]\n"
            "                              [--debug-subtitles]\n"
            "                              [--debug-yellow-highlight]\n"
            "                              [--authored-start]\n"
            "       dvd_av_threaded_test [device] --list-titles\n"
            "Defaults to /dev/sr0, title 2 / chapter 1.\n"
            "--authored-start        skip title/chapter jump; follow the disc\n"
            "                        First Play / VM (public launcher).\n"
            "--initial-video-skip N  discard N decoded frames after audio is\n"
            "                        ready, then present remaining frames with\n"
            "                        PTS shifted by -N*T (hold and stale).\n"
            "                        Default 0. N=1 = one source-frame advance.\n"
            "--video-advance-ms N    present video N ms earlier (buffered-YUV\n"
            "                        only). Added to presentation_vpts for PTS\n"
            "                        hold and stale recovery. Default 0.\n"
            "--uncapped-video-benchmark  decode + sws into the inactive DDR\n"
            "                        buffer with no PTS hold, stale drop, ACK\n"
            "                        wait, mailbox, or audio playback. Measures\n"
            "                        wall-clock frame production only.\n"
            "--buffered-video        decode + sws into a 25-frame cached RAM\n"
            "                        ring; a consumer memcpy's to DDR A/B.\n"
            "                        Default direct-DDR path is unchanged.\n"
            "--buffered-yuv-video    decode into a 25-frame AVFrame queue;\n"
            "                        consumer sws_scale YUV directly into\n"
            "                        inactive DDR. Default path unchanged.\n"
            "--perf-present-no-convert  timing diagnostic (requires\n"
            "                        --buffered-yuv-video). Prime A/B once,\n"
            "                        then skip per-frame sws_scale. PTS/stale,\n"
            "                        ACK, mailbox, decode, and MrAudio stay.\n"
            "                        Picture may freeze. Not a playback mode.\n"
            "--perf-sws-ddr          standalone sws_scale YUV420P→BGR0 into\n"
            "                        the inactive O_SYNC DDR buffer. Source\n"
            "                        is a synthetic cached PAL 720x576 frame.\n"
            "                        No DVD decode, mailbox, ACK, or playback.\n"
            "--buffered-yuv-phase-decode  experimental: do not start MPEG-2\n"
            "                        decode while consumer sws_scale is writing\n"
            "                        uncached DDR. Requires --buffered-yuv-video.\n"
            "                        Producer may refill during ACK/PTS waits.\n"
            "--fpga-yuv420           experimental: copy planar YUV420P into the\n"
            "                        inactive A/B slot and set mailbox bit1 so\n"
            "                        FPGA converts BT.601. Skips sws_scale and\n"
            "                        ARM subtitle/menu BGR overlays. Requires\n"
            "                        DVD_FPGA_YUV420_Test.rbf. Default off =\n"
            "                        legacy BGR0 unchanged.\n"
            "--debug-stats           verbose instrumentation (ACK, timings,\n"
            "                        queues, stale distributions, threads).\n"
            "                        Default off; normal output is compact.\n"
            "--debug-spu             log menu SPU/HLI decode (no per-frame spam).\n"
            "--debug-subtitles       log movie subtitle stream/SPU/DCSQ events\n"
            "                        (no per-pixel spam).\n"
            "--debug-yellow-highlight  restore the old yellow button rectangle\n"
            "                        instead of authored SPU/HLI overlay.\n");
}

static int parse_positive_int(const char *s, int *out)
{
    char *end = NULL;
    long v;
    if (!s || !*s)
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || !end || *end || v < 1 || v > 99)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_nonneg_int(const char *s, int *out)
{
    char *end = NULL;
    long v;
    if (!s || !*s)
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || !end || *end || v < 0 || v > 99)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_ms_int(const char *s, int *out)
{
    char *end = NULL;
    long v;
    if (!s || !*s)
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || !end || *end || v < 0 || v > 500)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_cli(int argc, char **argv, Cli *cli)
{
    memset(cli, 0, sizeof(*cli));
    cli->device = "/dev/sr0";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list-titles")) {
            cli->list_titles = 1;
            continue;
        }
        if (!strcmp(argv[i], "--title")) {
            if (i + 1 >= argc || parse_positive_int(argv[++i], &cli->title)) {
                fprintf(stderr, "--title requires an integer N >= 1\n");
                return -1;
            }
            continue;
        }
        if (!strcmp(argv[i], "--chapter")) {
            if (i + 1 >= argc || parse_positive_int(argv[++i], &cli->chapter)) {
                fprintf(stderr, "--chapter requires an integer N >= 1\n");
                return -1;
            }
            continue;
        }
        if (!strcmp(argv[i], "--initial-video-skip")) {
            if (i + 1 >= argc ||
                parse_nonneg_int(argv[++i], &cli->initial_video_skip)) {
                fprintf(stderr,
                        "--initial-video-skip requires an integer N >= 0\n");
                return -1;
            }
            continue;
        }
        if (!strcmp(argv[i], "--video-advance-ms")) {
            if (i + 1 >= argc ||
                parse_ms_int(argv[++i], &cli->video_advance_ms)) {
                fprintf(stderr,
                        "--video-advance-ms requires an integer N in 0..500\n");
                return -1;
            }
            continue;
        }
        if (!strcmp(argv[i], "--uncapped-video-benchmark")) {
            cli->uncapped_video_benchmark = 1;
            continue;
        }
        if (!strcmp(argv[i], "--buffered-video")) {
            cli->buffered_video = 1;
            continue;
        }
        if (!strcmp(argv[i], "--buffered-yuv-video")) {
            cli->buffered_yuv = 1;
            continue;
        }
        if (!strcmp(argv[i], "--perf-present-no-convert")) {
            cli->perf_present_no_convert = 1;
            continue;
        }
        if (!strcmp(argv[i], "--perf-sws-ddr")) {
            cli->perf_sws_ddr = 1;
            continue;
        }
        if (!strcmp(argv[i], "--buffered-yuv-phase-decode")) {
            cli->phase_decode = 1;
            continue;
        }
        if (!strcmp(argv[i], "--fpga-yuv420")) {
            cli->fpga_yuv420 = 1;
            continue;
        }
        if (!strcmp(argv[i], "--debug-stats")) {
            cli->debug_stats = 1;
            continue;
        }
        if (!strcmp(argv[i], "--debug-spu")) {
            cli->debug_spu = 1;
            continue;
        }
        if (!strcmp(argv[i], "--debug-subtitles")) {
            cli->debug_subtitles = 1;
            continue;
        }
        if (!strcmp(argv[i], "--debug-yellow-highlight")) {
            cli->debug_yellow_highlight = 1;
            continue;
        }
        if (!strcmp(argv[i], "--authored-start")) {
            cli->authored_start = 1;
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage();
            return -1;
        }
        cli->device = argv[i];
    }
    if (cli->list_titles && (cli->title || cli->chapter || cli->authored_start)) {
        fprintf(stderr, "--list-titles cannot be combined with --title/--chapter/--authored-start\n");
        return -1;
    }
    if (cli->authored_start && (cli->title || cli->chapter)) {
        fprintf(stderr, "--authored-start cannot be combined with --title/--chapter\n");
        return -1;
    }
    if (cli->chapter && !cli->title) {
        fprintf(stderr, "--chapter requires --title\n");
        return -1;
    }
    if (cli->buffered_video && cli->buffered_yuv) {
        fprintf(stderr,
                "--buffered-video cannot be combined with "
                "--buffered-yuv-video\n");
        return -1;
    }
    if (cli->uncapped_video_benchmark &&
        (cli->buffered_video || cli->buffered_yuv)) {
        fprintf(stderr,
                "--buffered-video / --buffered-yuv-video cannot be combined "
                "with --uncapped-video-benchmark\n");
        return -1;
    }
    if (cli->video_advance_ms && !cli->buffered_yuv) {
        fprintf(stderr,
                "--video-advance-ms is temporary and only valid with "
                "--buffered-yuv-video\n");
        return -1;
    }
    if (cli->perf_present_no_convert && !cli->buffered_yuv) {
        fprintf(stderr,
                "--perf-present-no-convert requires --buffered-yuv-video\n");
        return -1;
    }
    if (cli->perf_present_no_convert && cli->uncapped_video_benchmark) {
        fprintf(stderr,
                "--perf-present-no-convert cannot be combined with "
                "--uncapped-video-benchmark\n");
        return -1;
    }
    if (cli->perf_sws_ddr &&
        (cli->buffered_yuv || cli->buffered_video ||
         cli->uncapped_video_benchmark || cli->perf_present_no_convert ||
         cli->phase_decode)) {
        fprintf(stderr,
                "--perf-sws-ddr cannot be combined with playback or "
                "ACK-isolation flags\n");
        return -1;
    }
    if (cli->phase_decode && !cli->buffered_yuv) {
        fprintf(stderr,
                "--buffered-yuv-phase-decode requires --buffered-yuv-video\n");
        return -1;
    }
    if (cli->phase_decode &&
        (cli->perf_present_no_convert || cli->uncapped_video_benchmark)) {
        fprintf(stderr,
                "--buffered-yuv-phase-decode cannot be combined with "
                "--perf-present-no-convert or --uncapped-video-benchmark\n");
        return -1;
    }
    if (cli->fpga_yuv420 &&
        (cli->perf_present_no_convert || cli->perf_sws_ddr ||
         cli->buffered_video || cli->uncapped_video_benchmark ||
         cli->phase_decode)) {
        fprintf(stderr,
                "--fpga-yuv420 cannot be combined with isolation, "
                "buffered-video, uncapped, or phase-decode flags\n");
        return -1;
    }
    return 0;
}

static int list_dvd_titles(dvdnav_t *nav)
{
    int32_t ntitles = 0;
    if (dvdnav_get_number_of_titles(nav, &ntitles) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "dvdnav_get_number_of_titles failed: %s\n",
                dvdnav_err_to_string(nav));
        return -1;
    }
    fprintf(stderr, "Titles on disc: %d\n", (int)ntitles);
    for (int32_t t = 1; t <= ntitles; t++) {
        int32_t parts = 0;
        if (dvdnav_get_number_of_parts(nav, t, &parts) != DVDNAV_STATUS_OK)
            fprintf(stderr, "  Title %2d:  chapter count unavailable\n", (int)t);
        else
            fprintf(stderr, "  Title %2d:  %d chapter(s)\n", (int)t, (int)parts);
    }
    return 0;
}

static int jump_to_title(dvdnav_t *nav, const Cli *cli, int32_t ntitles)
{
    int32_t parts = -1;
    if (cli->title < 1 || cli->title > ntitles) {
        fprintf(stderr, "Title %d is out of range (1..%d)\n",
                cli->title, (int)ntitles);
        return -1;
    }
    if (dvdnav_get_number_of_parts(nav, cli->title, &parts) == DVDNAV_STATUS_OK)
        dbg("Chapters in title %d: %d\n", cli->title, (int)parts);
    if (cli->chapter) {
        if (parts > 0 && cli->chapter > parts) {
            fprintf(stderr, "Chapter %d is out of range for title %d (1..%d)\n",
                    cli->chapter, cli->title, (int)parts);
            return -1;
        }
        if (dvdnav_part_play(nav, cli->title, cli->chapter) != DVDNAV_STATUS_OK) {
            fprintf(stderr, "dvdnav_part_play(%d, %d) failed: %s\n",
                    cli->title, cli->chapter, dvdnav_err_to_string(nav));
            return -1;
        }
    } else if (dvdnav_title_play(nav, cli->title) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "dvdnav_title_play(%d) failed: %s\n",
                cli->title, dvdnav_err_to_string(nav));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Framebuffer                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *fb_a, *fb_b;
    volatile uint32_t *mbox;
    int mem_fd, fb_fd, vsync_ok;
    int64_t vsync_period_us, vbl_origin_us, margin_us;
} FBPair;

static int overlaps_system_ram(unsigned long base, size_t size)
{
    FILE *fp = fopen("/proc/iomem", "r");
    char line[256];
    int checked = 0, overlap = 0;
    if (!fp)
        return -1;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long long start = 0, end = 0;
        if (!strstr(line, "System RAM"))
            continue;
        if (sscanf(line, " %llx-%llx :", &start, &end) != 2)
            continue;
        if (start == 0 && end == 0)
            continue;
        checked = 1;
        dbg("  System RAM: 0x%09llx-0x%09llx\n", start, end);
        if (base <= end && (base + size - 1) >= start)
            overlap = 1;
    }
    fclose(fp);
    return checked ? overlap : -1;
}

static int check_range(unsigned long base, size_t size)
{
    dbg("Target range: 0x%08lx-0x%08lx (%zu bytes)\n",
        base, base + size - 1, size);
    int ov = overlaps_system_ram(base, size);
    if (ov > 0) {
        fprintf(stderr, "FAIL: 0x%08lx overlaps Linux System RAM.\n", base);
        return -1;
    }
    if (ov < 0)
        dbg("WARNING: could not verify via /proc/iomem.\n");
    else
        dbg("OK: no overlap with System RAM.\n");
    return 0;
}

static int wait_vsync(int fb_fd)
{
    int arg = 0;
    if (fb_fd >= 0 && ioctl(fb_fd, FBIO_WAITFORVSYNC, &arg) == 0)
        return 0;
    usleep(20000);
    return -1;
}

static void wait_n_vsync(int fb_fd, int n)
{
    for (int i = 0; i < n; i++)
        wait_vsync(fb_fd);
}

static int map_double_fb(FBPair *fb)
{
    stage = 6;
    memset(fb, 0, sizeof(*fb));
    fb->mem_fd = -1;
    fb->fb_fd = -1;
    dbg("\nMapping MISTER_FB double buffers + mailbox:\n"
        "  A=0x%08lx  B=0x%08lx  mailbox=0x%08lx\n"
        "Leave OSD Buffer on A.\n",
        FB_A_PHYS, FB_B_PHYS, MB_PHYS);
    if (check_range(FB_A_PHYS, FB_SIZE) < 0 ||
        check_range(FB_B_PHYS, FB_SIZE) < 0 ||
        check_range(MB_PHYS, MB_MAP_SIZE) < 0)
        return -1;
    fb->mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fb->mem_fd < 0) {
        fprintf(stderr, "open /dev/mem: %s (need root)\n", strerror(errno));
        return -1;
    }
    fb->fb_a = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fb->mem_fd, (off_t)FB_A_PHYS);
    fb->fb_b = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fb->mem_fd, (off_t)FB_B_PHYS);
    void *mb_map = mmap(NULL, MB_MAP_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fb->mem_fd, (off_t)MB_PHYS);
    if (fb->fb_a == MAP_FAILED || fb->fb_b == MAP_FAILED ||
        mb_map == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return -1;
    }
    fb->mbox = mb_map;
    fb->fb_fd = open("/dev/fb0", O_RDWR);
    if (fb->fb_fd < 0) {
        dbg("open /dev/fb0: %s (will sleep 20 ms)\n", strerror(errno));
        fb->vsync_ok = 0;
    } else if (wait_vsync(fb->fb_fd) < 0) {
        dbg("FBIO_WAITFORVSYNC failed; using 20 ms sleep\n");
        fb->vsync_ok = 0;
    } else {
        fb->vsync_ok = 1;
        dbg("Using FBIO_WAITFORVSYNC.\n");
    }
    fb->mbox[0] = 0;
    wait_n_vsync(fb->fb_fd, STARTUP_DISPLAY_VBLS);
    wait_vsync(fb->fb_fd);
    int64_t t0 = av_gettime_relative();
    for (int i = 0; i < VSYNC_SAMPLE_INTERVALS; i++)
        wait_vsync(fb->fb_fd);
    int64_t t1 = av_gettime_relative();
    fb->vsync_period_us = (t1 - t0) / VSYNC_SAMPLE_INTERVALS;
    if (fb->vsync_period_us <= 0)
        fb->vsync_period_us = 20000;
    fb->vbl_origin_us = t1;
    fb->margin_us = MAILBOX_POLL_MARGIN_US;
    dbg("HDMI vsync (diagnostic only, not the flip grid): "
        "%d intervals in %.3f ms  ->  %.3f ms/period (%.3f Hz)\n",
        VSYNC_SAMPLE_INTERVALS, (double)(t1 - t0) / 1000.0,
        (double)fb->vsync_period_us / 1000.0,
        1e6 / (double)fb->vsync_period_us);
    return 0;
}

static void unmap_double_fb(FBPair *fb)
{
    if (fb->fb_a && fb->fb_a != MAP_FAILED)
        munmap(fb->fb_a, FB_SIZE);
    if (fb->fb_b && fb->fb_b != MAP_FAILED)
        munmap(fb->fb_b, FB_SIZE);
    if (fb->mbox)
        munmap((void *)fb->mbox, MB_MAP_SIZE);
    if (fb->mem_fd >= 0)
        close(fb->mem_fd);
    if (fb->fb_fd >= 0)
        close(fb->fb_fd);
}

static uint64_t peek_mbox_status(const FBPair *fb)
{
    volatile uint64_t *st =
        (volatile uint64_t *)((uint8_t *)fb->mbox + JOY_OFF);
    return *st;
}

static uint64_t peek_dvd_settings(const FBPair *fb)
{
    volatile uint64_t *st =
        (volatile uint64_t *)((uint8_t *)fb->mbox + SET_OFF);
    return *st;
}

static void poke_dvd_control(const FBPair *fb, uint64_t word)
{
    volatile uint64_t *st =
        (volatile uint64_t *)((uint8_t *)fb->mbox + CTL_OFF);
    *st = word;
    __sync_synchronize();
}

/* Circular OSD encoding: raw 0=0ms, 1..10=+10..+100, 11..20=-100..-10. */
static int av_sync_raw_to_ms(unsigned raw)
{
    raw &= 31u;
    if (raw > 20)
        return 0;
    if (raw <= 10)
        return (int)raw * 10;
    return ((int)raw - 21) * 10;
}

static int dvd_fpga_probe_v1(const FBPair *fb)
{
    uint64_t a, b;
    unsigned seq_a, seq_b;
    int i;

    if (!fb || !fb->mbox)
        return 0;
    a = peek_dvd_settings(fb);
    if ((uint32_t)(a >> 32) != SET_MAGIC)
        return 0;
    if (((a >> 24) & 0xffu) != SET_VER)
        return 0;
    seq_a = (unsigned)((a >> 16) & 0xffu);
    for (i = 0; i < 8; i++) {
        av_usleep(2000);
        b = peek_dvd_settings(fb);
        if ((uint32_t)(b >> 32) != SET_MAGIC)
            return 0;
        seq_b = (unsigned)((b >> 16) & 0xffu);
        if (seq_b != seq_a)
            return 1;
    }
    return 0;
}

static int dvd_fpga_yuv_cap(const FBPair *fb)
{
    uint64_t w;

    if (!fb || !fb->mbox)
        return 0;
    w = peek_dvd_settings(fb);
    if ((uint32_t)(w >> 32) != SET_MAGIC)
        return 0;
    if (((w >> 24) & 0xffu) != SET_VER)
        return 0;
    return (int)((w >> SET_YUV_CAP_BIT) & 1u);
}

static uint32_t mailbox_ab_word(int yuv_mode, int ab)
{
    uint32_t w = (uint32_t)(ab & 1);
    if (yuv_mode)
        w |= (1u << MB_YUV_BIT);
    return w;
}

static void dvd_fpga_write_source(const FBPair *fb, unsigned src)
{
    uint64_t w;

    if (!fb || !fb->mbox)
        return;
    if (src > FPGA_SRC_PAL)
        src = FPGA_SRC_UNKNOWN;
    w = ((uint64_t)CTL_MAGIC << 32) | (src & 3u);
    poke_dvd_control(fb, w);
}

static int read_display_buf(const FBPair *fb, int *out)
{
    uint64_t w = peek_mbox_status(fb);
    uint32_t magic = (uint32_t)(w >> 32);
    uint32_t joy = (uint32_t)w;

    if (magic != JOY_MAGIC)
        return -1;
    *out = (int)((joy >> DISP_BUF_BIT) & 1u);
    return 0;
}

typedef struct {
    int up, down, left, right;
    int confirm, cancel, menu, play_pause, next, previous;
} ControllerState;

typedef struct {
    uint32_t prev_bits;
    int primed;
    int64_t dir_held_us[4];
    int64_t dir_last_us[4];
    int64_t cancel_hold_start_us;
    int cancel_exit_triggered;
    int magic_ok;
    int magic_logged;
    unsigned long events;
} ControllerPad;

static void controller_from_bits(uint32_t bits, ControllerState *s)
{
    memset(s, 0, sizeof(*s));
    s->right = !!(bits & (1u << JOY_BIT_RIGHT));
    s->left = !!(bits & (1u << JOY_BIT_LEFT));
    s->down = !!(bits & (1u << JOY_BIT_DOWN));
    s->up = !!(bits & (1u << JOY_BIT_UP));
    s->confirm = !!(bits & (1u << JOY_BIT_SELECT));
    s->cancel = !!(bits & (1u << JOY_BIT_BACK));
    s->play_pause = !!(bits & (1u << JOY_BIT_PLAYPAUSE));
    s->menu = !!(bits & (1u << JOY_BIT_MENU));
    s->previous = !!(bits & (1u << JOY_BIT_PREV));
    s->next = !!(bits & (1u << JOY_BIT_NEXT));
}

static int read_joystick_buttons(const FBPair *fb, uint32_t *buttons_out)
{
    volatile uint64_t *st =
        (volatile uint64_t *)((uint8_t *)fb->mbox + JOY_OFF);
    uint64_t w = *st;
    uint32_t magic = (uint32_t)(w >> 32);
    uint32_t joy = (uint32_t)w;

    if (magic != JOY_MAGIC)
        return -1;
    *buttons_out = joy & JOY_BTN_MASK;
    return 0;
}

static void controller_emit(const char *name, unsigned long *events, Player *p,
                            NavCmd cmd)
{
    fprintf(stderr, "CONTROLLER: %s\n", name);
    (*events)++;
    if (p && cmd != NAVCMD_NONE)
        navq_post(p, cmd);
}

/* Rising-edge actions. D-pad also auto-repeats after CTRL_REPEAT_DELAY_US. */
static void controller_poll(ControllerPad *pad, uint32_t bits, int64_t now_us,
                            Player *p)
{
    ControllerState now, prev;
    int dirs[4];
    int i;

    controller_from_bits(bits, &now);
    if (!pad->primed) {
        pad->prev_bits = bits;
        pad->primed = 1;
        for (i = 0; i < 4; i++) {
            pad->dir_held_us[i] = 0;
            pad->dir_last_us[i] = 0;
        }
        return;
    }
    controller_from_bits(pad->prev_bits, &prev);

    if (now.up && !prev.up)
        controller_emit("UP", &pad->events, p, NAVCMD_UP);
    if (now.down && !prev.down)
        controller_emit("DOWN", &pad->events, p, NAVCMD_DOWN);
    if (now.left && !prev.left)
        controller_emit("LEFT", &pad->events, p, NAVCMD_LEFT);
    if (now.right && !prev.right)
        controller_emit("RIGHT", &pad->events, p, NAVCMD_RIGHT);
    if (now.confirm && !prev.confirm)
        controller_emit("CONFIRM", &pad->events, p, NAVCMD_ACTIVATE);
    /*
     * CANCEL is rising-edge only — it is not in the D-pad auto-repeat
     * loop. A tap posts one NAVCMD_CANCEL. A held B does not flood
     * dvdnav; after CANCEL_EXIT_HOLD_US it requests the same shutdown
     * as SIGINT/SIGTERM (g_interrupt).
     */
    if (now.cancel && !prev.cancel)
        controller_emit("CANCEL", &pad->events, p, NAVCMD_CANCEL);
    if (now.cancel) {
        if (!pad->cancel_hold_start_us)
            pad->cancel_hold_start_us = now_us;
        if (!pad->cancel_exit_triggered &&
            now_us - pad->cancel_hold_start_us >= CANCEL_EXIT_HOLD_US) {
            pad->cancel_exit_triggered = 1;
            fprintf(stderr, "CONTROLLER: EXIT hold 3000ms\n");
            fprintf(stderr, "PLAYER: returning to launcher\n");
            g_interrupt = 1;
            pause_wake(p);
        }
    } else {
        pad->cancel_hold_start_us = 0;
        pad->cancel_exit_triggered = 0;
    }
    if (now.menu && !prev.menu)
        controller_emit("MENU", &pad->events, p, NAVCMD_MENU);
    if (now.play_pause && !prev.play_pause)
        controller_emit("PLAY_PAUSE", &pad->events, p, NAVCMD_PLAY_PAUSE);
    /*
     * AUDIO NEXT chord before individual PREV/NEXT: one rising
     * PREV+NEXT = one cycle. Holding both does not repeat. While the
     * chord is held, chapter PREV/NEXT are suppressed.
     */
    if (now.previous && now.next) {
        if (!(prev.previous && prev.next))
            controller_emit("AUDIO NEXT", &pad->events, p, NAVCMD_AUDIO_NEXT);
    } else {
        if (now.next && !prev.next)
            controller_emit("NEXT", &pad->events, p, NAVCMD_NEXT_CHAPTER);
        if (now.previous && !prev.previous)
            controller_emit("PREVIOUS", &pad->events, p,
                            NAVCMD_PREVIOUS_CHAPTER);
    }

    dirs[0] = now.up;
    dirs[1] = now.down;
    dirs[2] = now.left;
    dirs[3] = now.right;
    {
        static const char *const dir_names[4] = {
            "UP", "DOWN", "LEFT", "RIGHT"
        };
        static const NavCmd dir_cmds[4] = {
            NAVCMD_UP, NAVCMD_DOWN, NAVCMD_LEFT, NAVCMD_RIGHT
        };
        for (i = 0; i < 4; i++) {
            if (dirs[i]) {
                if (!pad->dir_held_us[i]) {
                    pad->dir_held_us[i] = now_us;
                    pad->dir_last_us[i] = now_us;
                } else if (now_us - pad->dir_held_us[i] >= CTRL_REPEAT_DELAY_US &&
                           now_us - pad->dir_last_us[i] >= CTRL_REPEAT_RATE_US) {
                    controller_emit(dir_names[i], &pad->events, p, dir_cmds[i]);
                    pad->dir_last_us[i] = now_us;
                }
            } else {
                pad->dir_held_us[i] = 0;
                pad->dir_last_us[i] = 0;
            }
        }
    }
    pad->prev_bits = bits;
}

/* ------------------------------------------------------------------ */
/* Bounded packet queue                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    AVPacket *pkts;
    int cap, head, tail, count;
    int eof, quit;
    int depth_min, depth_max, depth_n;
    int64_t depth_sum;
    int64_t block_us;
    unsigned long full_n;
    unsigned long pushed, popped;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} PktQ;

static int pktq_init(PktQ *q, int cap)
{
    memset(q, 0, sizeof(*q));
    q->cap = cap;
    q->pkts = av_calloc((size_t)cap, sizeof(*q->pkts));
    if (!q->pkts)
        return -1;
    q->depth_min = cap;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return 0;
}

static void pktq_note_depth(PktQ *q)
{
    if (!q->depth_n || q->count < q->depth_min)
        q->depth_min = q->count;
    if (q->count > q->depth_max)
        q->depth_max = q->count;
    q->depth_sum += q->count;
    q->depth_n++;
}

static void pause_thr_set(int *slot, int st)
{
    if (slot)
        __atomic_store_n(slot, st, __ATOMIC_RELAXED);
}

static int pause_thr_get(const int *slot)
{
    return slot ? __atomic_load_n(slot, __ATOMIC_RELAXED) : THR_RUN;
}

static const char *pause_thr_name(int st)
{
    switch (st) {
    case THR_PAUSE:
        return "wait-pause";
    case THR_QFULL:
        return "wait-queue-full";
    case THR_QEMPTY:
        return "wait-queue-empty";
    case THR_PREFILL:
        return "wait-prefill";
    case THR_CLOCK:
        return "wait-clock";
    default:
        return "run";
    }
}

static void pktq_wait_timeout(pthread_cond_t *cv, pthread_mutex_t *mu)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 200000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(cv, mu, &ts);
}

static void pktq_push(PktQ *q, AVPacket *src)
{
    int64_t t0 = 0;
    pthread_mutex_lock(&q->mu);
    while (q->count >= q->cap && !q->quit && !g_interrupt) {
        if (!t0) {
            t0 = av_gettime_relative();
            q->full_n++;
        }
        pktq_wait_timeout(&q->not_full, &q->mu);
    }
    if (g_interrupt)
        q->quit = 1;
    if (t0)
        q->block_us += av_gettime_relative() - t0;
    if (q->quit) {
        pthread_mutex_unlock(&q->mu);
        return;
    }
    av_packet_ref(&q->pkts[q->tail], src);
    q->pkts[q->tail].opaque = src->opaque;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    q->pushed++;
    pktq_note_depth(q);
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

static void pktq_push_marker(PktQ *q, void *mark)
{
    AVPacket *tmp = av_packet_alloc();

    if (!tmp)
        return;
    tmp->opaque = mark;
    pktq_push(q, tmp);
    av_packet_free(&tmp);
}

/* 1 = packet, 0 = eof/quit */
static int pktq_pop(PktQ *q, AVPacket *dst)
{
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->eof && !q->quit && !g_interrupt)
        pktq_wait_timeout(&q->not_empty, &q->mu);
    if (g_interrupt)
        q->quit = 1;
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mu);
        return 0;
    }
    av_packet_move_ref(dst, &q->pkts[q->head]);
    q->head = (q->head + 1) % q->cap;
    q->count--;
    q->popped++;
    pktq_note_depth(q);
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return 1;
}

static void pktq_eof(PktQ *q)
{
    pthread_mutex_lock(&q->mu);
    q->eof = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

static void pktq_quit(PktQ *q)
{
    pthread_mutex_lock(&q->mu);
    q->quit = 1;
    q->eof = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);
}

static int pktq_count(PktQ *q)
{
    int n;
    pthread_mutex_lock(&q->mu);
    n = q->count;
    pthread_mutex_unlock(&q->mu);
    return n;
}

/* Drop queued compressed packets. Does not set eof (decoders stay alive). */
static int pktq_flush(PktQ *q)
{
    int n, i;

    pthread_mutex_lock(&q->mu);
    n = q->count;
    for (i = 0; i < n; i++)
        av_packet_unref(&q->pkts[(q->head + i) % q->cap]);
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return n;
}

static void pktq_free(PktQ *q)
{
    if (!q->pkts)
        return;
    for (int i = 0; i < q->cap; i++)
        av_packet_unref(&q->pkts[i]);
    av_free(q->pkts);
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/* ------------------------------------------------------------------ */
/* Cached BGR0 video ring (--buffered-video only)                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *bgr0;
    int64_t raw_vpts_us;
} VidSlot;

typedef struct {
    VidSlot slots[VIDEO_BUFFER_FRAMES];
    int cap, head, tail, count;
    int eof, quit, inited;
    int depth_min, depth_max, depth_n;
    int64_t depth_sum;
    int play_depth_min, play_depth_n;
    int64_t play_depth_sum;
    unsigned long full_n, empty_n, cap_hits;
    int64_t full_block_us, empty_wait_us;
    unsigned long pushed, popped;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} VidRing;

static void vidring_note_depth(VidRing *r, int playing)
{
    if (!r->depth_n || r->count < r->depth_min)
        r->depth_min = r->count;
    if (r->count > r->depth_max)
        r->depth_max = r->count;
    r->depth_sum += r->count;
    r->depth_n++;
    if (playing) {
        if (!r->play_depth_n || r->count < r->play_depth_min)
            r->play_depth_min = r->count;
        r->play_depth_sum += r->count;
        r->play_depth_n++;
    }
}

static int vidring_init(VidRing *r)
{
    int i;

    memset(r, 0, sizeof(*r));
    r->cap = VIDEO_BUFFER_FRAMES;
    r->depth_min = r->cap;
    r->play_depth_min = r->cap;
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->not_empty, NULL);
    pthread_cond_init(&r->not_full, NULL);
    for (i = 0; i < r->cap; i++) {
        void *mem = NULL;
        if (posix_memalign(&mem, 64, FB_SIZE) != 0) {
            fprintf(stderr, "vidring posix_memalign failed\n");
            while (i-- > 0)
                free(r->slots[i].bgr0);
            pthread_mutex_destroy(&r->mu);
            pthread_cond_destroy(&r->not_empty);
            pthread_cond_destroy(&r->not_full);
            memset(r, 0, sizeof(*r));
            return -1;
        }
        r->slots[i].bgr0 = mem;
        r->slots[i].raw_vpts_us = AV_NOPTS_VALUE;
    }
    r->inited = 1;
    dbg("Cached video ring: %d frames × %zu bytes  (%.1f MiB, BGR0 %dx%d)\n",
        r->cap, FB_SIZE,
        (r->cap * (double)FB_SIZE) / (1024.0 * 1024.0),
        FB_W, FB_H);
    return 0;
}

static void vidring_eof(VidRing *r)
{
    if (!r->inited)
        return;
    pthread_mutex_lock(&r->mu);
    r->eof = 1;
    pthread_cond_broadcast(&r->not_empty);
    pthread_mutex_unlock(&r->mu);
}

static void vidring_quit(VidRing *r)
{
    if (!r->inited)
        return;
    pthread_mutex_lock(&r->mu);
    r->quit = 1;
    r->eof = 1;
    pthread_cond_broadcast(&r->not_empty);
    pthread_cond_broadcast(&r->not_full);
    pthread_mutex_unlock(&r->mu);
}

static int vidring_count(VidRing *r)
{
    int n;
    if (!r->inited)
        return 0;
    pthread_mutex_lock(&r->mu);
    n = r->count;
    pthread_mutex_unlock(&r->mu);
    return n;
}

/* 1 = slot reserved for produce (not yet visible to consumer). */
static int vidring_begin_produce(VidRing *r, int *idx_out, Player *p)
{
    int64_t t0 = 0;

    pthread_mutex_lock(&r->mu);
    while (r->count >= r->cap && !r->quit && !g_interrupt) {
        if (!t0) {
            t0 = av_gettime_relative();
            r->full_n++;
        }
        if (p && pause_is_held(p)) {
            pthread_mutex_unlock(&r->mu);
            pause_wait_unheld(p);
            pthread_mutex_lock(&r->mu);
            continue;
        }
        pktq_wait_timeout(&r->not_full, &r->mu);
    }
    if (g_interrupt)
        r->quit = 1;
    if (t0)
        r->full_block_us += av_gettime_relative() - t0;
    if (r->quit) {
        pthread_mutex_unlock(&r->mu);
        return 0;
    }
    *idx_out = r->tail;
    pthread_mutex_unlock(&r->mu);
    return 1;
}

static void vidring_commit_produce(VidRing *r, int idx, int64_t raw_vpts_us,
                                   int playing)
{
    pthread_mutex_lock(&r->mu);
    r->slots[idx].raw_vpts_us = raw_vpts_us;
    r->tail = (idx + 1) % r->cap;
    r->count++;
    r->pushed++;
    if (r->count >= r->cap)
        r->cap_hits++;
    vidring_note_depth(r, playing);
    pthread_cond_signal(&r->not_empty);
    pthread_mutex_unlock(&r->mu);
}

/* 1 = filled slot; pixels remain owned until vidring_release. 0 = eof. */
static int vidring_acquire_filled(VidRing *r, int *idx_out, int64_t *pts_out,
                                  uint8_t **pix_out, int playing)
{
    int64_t t0 = 0;

    pthread_mutex_lock(&r->mu);
    while (r->count == 0 && !r->eof && !r->quit && !g_interrupt) {
        if (!t0) {
            t0 = av_gettime_relative();
            if (playing)
                r->empty_n++;
        }
        pktq_wait_timeout(&r->not_empty, &r->mu);
    }
    if (g_interrupt)
        r->quit = 1;
    if (t0)
        r->empty_wait_us += av_gettime_relative() - t0;
    if (r->count == 0) {
        pthread_mutex_unlock(&r->mu);
        return 0;
    }
    *idx_out = r->head;
    *pts_out = r->slots[r->head].raw_vpts_us;
    *pix_out = r->slots[r->head].bgr0;
    pthread_mutex_unlock(&r->mu);
    return 1;
}

static void vidring_release(VidRing *r, int playing)
{
    pthread_mutex_lock(&r->mu);
    r->head = (r->head + 1) % r->cap;
    r->count--;
    r->popped++;
    vidring_note_depth(r, playing);
    pthread_cond_signal(&r->not_full);
    pthread_mutex_unlock(&r->mu);
}

static void vidring_free(VidRing *r)
{
    int i;
    if (!r->inited)
        return;
    for (i = 0; i < r->cap; i++)
        free(r->slots[i].bgr0);
    pthread_mutex_destroy(&r->mu);
    pthread_cond_destroy(&r->not_empty);
    pthread_cond_destroy(&r->not_full);
    memset(r, 0, sizeof(*r));
}

/* ------------------------------------------------------------------ */
/* Decoded YUV AVFrame ring (--buffered-yuv-video only)                */
/* One AVFrame container per slot; av_frame_ref() holds decoder        */
/* buffers. No YUV plane copies. Producer never sws/ACK/DDR.           */
/* ------------------------------------------------------------------ */

typedef struct {
    AVFrame *yuv;
    int64_t raw_vpts_us;
    int width, height;
    enum AVPixelFormat format;
    unsigned nav_gen;
    int in_menu;
} YuvSlot;

typedef struct {
    YuvSlot slots[VIDEO_BUFFER_FRAMES];
    int cap, head, tail, count;
    int eof, quit, inited;
    int held;
    unsigned epoch;
    int depth_min, depth_max, depth_n;
    int64_t depth_sum;
    int play_depth_min, play_depth_n;
    int64_t play_depth_sum;
    unsigned long full_n, empty_n, cap_hits;
    int64_t full_block_us, empty_wait_us;
    unsigned long pushed, popped;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} YuvRing;

static void yuvring_note_depth(YuvRing *r, int playing)
{
    if (!r->depth_n || r->count < r->depth_min)
        r->depth_min = r->count;
    if (r->count > r->depth_max)
        r->depth_max = r->count;
    r->depth_sum += r->count;
    r->depth_n++;
    if (playing) {
        if (!r->play_depth_n || r->count < r->play_depth_min)
            r->play_depth_min = r->count;
        r->play_depth_sum += r->count;
        r->play_depth_n++;
    }
}

static int yuvring_init(YuvRing *r)
{
    int i;

    memset(r, 0, sizeof(*r));
    r->cap = VIDEO_BUFFER_FRAMES;
    r->depth_min = r->cap;
    r->play_depth_min = r->cap;
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->not_empty, NULL);
    pthread_cond_init(&r->not_full, NULL);
    for (i = 0; i < r->cap; i++) {
        r->slots[i].yuv = av_frame_alloc();
        if (!r->slots[i].yuv) {
            fprintf(stderr, "yuvring av_frame_alloc failed\n");
            while (i-- > 0)
                av_frame_free(&r->slots[i].yuv);
            pthread_mutex_destroy(&r->mu);
            pthread_cond_destroy(&r->not_empty);
            pthread_cond_destroy(&r->not_full);
            memset(r, 0, sizeof(*r));
            return -1;
        }
        r->slots[i].raw_vpts_us = AV_NOPTS_VALUE;
        r->slots[i].format = AV_PIX_FMT_NONE;
        r->slots[i].nav_gen = 0;
        r->slots[i].in_menu = 0;
    }
    r->inited = 1;
    dbg("Decoded YUV ring: %d AVFrame refs  (no plane copies; "
        "~%.1f MiB if 720x576 yuv420p; NTSC uses 720x480 of the same slots)\n",
        r->cap,
        (r->cap * 720.0 * 576.0 * 1.5) / (1024.0 * 1024.0));
    return 0;
}

static void yuvring_eof(YuvRing *r)
{
    if (!r->inited)
        return;
    pthread_mutex_lock(&r->mu);
    r->eof = 1;
    pthread_cond_broadcast(&r->not_empty);
    pthread_mutex_unlock(&r->mu);
}

static void yuvring_quit(YuvRing *r)
{
    if (!r->inited)
        return;
    pthread_mutex_lock(&r->mu);
    r->quit = 1;
    r->eof = 1;
    pthread_cond_broadcast(&r->not_empty);
    pthread_cond_broadcast(&r->not_full);
    pthread_mutex_unlock(&r->mu);
}

static int yuvring_count(YuvRing *r)
{
    int n;
    if (!r->inited)
        return 0;
    pthread_mutex_lock(&r->mu);
    n = r->count;
    pthread_mutex_unlock(&r->mu);
    return n;
}

static int yuvring_begin_produce(YuvRing *r, int *idx_out, unsigned *epoch_out,
                                 Player *p)
{
    int64_t t0 = 0;

    pthread_mutex_lock(&r->mu);
    while (r->count >= r->cap && !r->quit && !g_interrupt) {
        if (!t0) {
            t0 = av_gettime_relative();
            r->full_n++;
        }
        if (p && pause_is_held(p)) {
            pthread_mutex_unlock(&r->mu);
            pause_wait_unheld(p);
            pthread_mutex_lock(&r->mu);
            continue;
        }
        pktq_wait_timeout(&r->not_full, &r->mu);
    }
    if (g_interrupt)
        r->quit = 1;
    if (t0)
        r->full_block_us += av_gettime_relative() - t0;
    if (r->quit) {
        pthread_mutex_unlock(&r->mu);
        return 0;
    }
    *idx_out = r->tail;
    *epoch_out = r->epoch;
    pthread_mutex_unlock(&r->mu);
    return 1;
}

static int yuvring_commit_produce(YuvRing *r, int idx, int64_t raw_vpts_us,
                                  int playing, unsigned epoch, unsigned nav_gen,
                                  int in_menu)
{
    pthread_mutex_lock(&r->mu);
    if (epoch != r->epoch) {
        av_frame_unref(r->slots[idx].yuv);
        r->slots[idx].raw_vpts_us = AV_NOPTS_VALUE;
        r->slots[idx].format = AV_PIX_FMT_NONE;
        r->slots[idx].nav_gen = 0;
        r->slots[idx].in_menu = 0;
        pthread_mutex_unlock(&r->mu);
        return 0;
    }
    r->slots[idx].raw_vpts_us = raw_vpts_us;
    r->slots[idx].nav_gen = nav_gen;
    r->slots[idx].in_menu = in_menu ? 1 : 0;
    r->tail = (idx + 1) % r->cap;
    r->count++;
    r->pushed++;
    if (r->count >= r->cap)
        r->cap_hits++;
    yuvring_note_depth(r, playing);
    pthread_cond_signal(&r->not_empty);
    pthread_mutex_unlock(&r->mu);
    return 1;
}

/* 1 = filled slot; 2 = idle (check UI redraw); 0 = eof. */
static int yuvring_acquire_filled(YuvRing *r, AVFrame **fr_out, int64_t *pts_out,
                                  unsigned *gen_out, int *in_menu_out,
                                  int playing)
{
    int64_t t0 = 0;

    pthread_mutex_lock(&r->mu);
    if (r->count == 0 && !r->eof && !r->quit && !g_interrupt) {
        t0 = av_gettime_relative();
        if (playing)
            r->empty_n++;
        pktq_wait_timeout(&r->not_empty, &r->mu);
    }
    if (g_interrupt)
        r->quit = 1;
    if (t0)
        r->empty_wait_us += av_gettime_relative() - t0;
    if (r->count == 0) {
        int done = r->eof || r->quit;
        pthread_mutex_unlock(&r->mu);
        return done ? 0 : 2;
    }
    r->held = 1;
    *fr_out = r->slots[r->head].yuv;
    *pts_out = r->slots[r->head].raw_vpts_us;
    if (gen_out)
        *gen_out = r->slots[r->head].nav_gen;
    if (in_menu_out)
        *in_menu_out = r->slots[r->head].in_menu;
    pthread_mutex_unlock(&r->mu);
    return 1;
}

static void yuvring_release(YuvRing *r, int playing)
{
    pthread_mutex_lock(&r->mu);
    av_frame_unref(r->slots[r->head].yuv);
    r->slots[r->head].raw_vpts_us = AV_NOPTS_VALUE;
    r->slots[r->head].format = AV_PIX_FMT_NONE;
    r->slots[r->head].nav_gen = 0;
    r->slots[r->head].in_menu = 0;
    r->head = (r->head + 1) % r->cap;
    r->count--;
    r->popped++;
    r->held = 0;
    yuvring_note_depth(r, playing);
    pthread_cond_signal(&r->not_full);
    pthread_mutex_unlock(&r->mu);
}

static int yuvring_flush(YuvRing *r)
{
    int dropped = 0;
    int i;

    if (!r->inited)
        return 0;
    pthread_mutex_lock(&r->mu);
    r->epoch++;
    if (r->held && r->count > 0) {
        for (i = 1; i < r->count; i++) {
            int s = (r->head + i) % r->cap;
            av_frame_unref(r->slots[s].yuv);
            r->slots[s].raw_vpts_us = AV_NOPTS_VALUE;
            r->slots[s].format = AV_PIX_FMT_NONE;
            r->slots[s].nav_gen = 0;
            r->slots[s].in_menu = 0;
            dropped++;
        }
        r->tail = (r->head + 1) % r->cap;
        r->count = 1;
    } else {
        for (i = 0; i < r->count; i++) {
            int s = (r->head + i) % r->cap;
            av_frame_unref(r->slots[s].yuv);
            r->slots[s].raw_vpts_us = AV_NOPTS_VALUE;
            r->slots[s].format = AV_PIX_FMT_NONE;
            r->slots[s].nav_gen = 0;
            r->slots[s].in_menu = 0;
            dropped++;
        }
        r->head = 0;
        r->tail = 0;
        r->count = 0;
        r->held = 0;
    }
    pthread_cond_broadcast(&r->not_empty);
    pthread_cond_broadcast(&r->not_full);
    pthread_mutex_unlock(&r->mu);
    return dropped;
}

static void yuvring_free(YuvRing *r)
{
    int i;
    if (!r->inited)
        return;
    for (i = 0; i < r->cap; i++)
        av_frame_free(&r->slots[i].yuv);
    pthread_mutex_destroy(&r->mu);
    pthread_cond_destroy(&r->not_empty);
    pthread_cond_destroy(&r->not_full);
    memset(r, 0, sizeof(*r));
}

/* ------------------------------------------------------------------ */
/* MrAudio (audio thread only, except clock snapshot)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int wr_fd, hw_pace;
    int64_t bytes_written, prime_bytes, bytes_origin, short_writes;
    int write_errors, polls;
    int rptr, wptr, fill, comp;
    char last_line[128];
    int64_t wall_origin;
    int64_t fill_min, fill_max, fill_sum;
    int fill_n;
    int in_underrun;
    int underruns;
    int64_t underrun_us, underrun_start;
    int playing;
} MrAudio;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t ready_cv;
    int64_t first_pts_us;
    int64_t elapsed_us;
    int64_t clock_us;
    int64_t last_apts_us;
    int fill;
    int ready;
    int hw_pace;
    unsigned epoch;
} AudioClock;

static void clock_init(AudioClock *c)
{
    memset(c, 0, sizeof(*c));
    c->first_pts_us = AV_NOPTS_VALUE;
    c->last_apts_us = AV_NOPTS_VALUE;
    c->clock_us = AV_NOPTS_VALUE;
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->ready_cv, NULL);
}

static void clock_publish(AudioClock *c, int64_t first_pts_us, int64_t elapsed_us,
                          int64_t last_apts_us, int fill, int hw_pace, int ready,
                          unsigned epoch)
{
    pthread_mutex_lock(&c->mu);
    if (epoch != c->epoch) {
        pthread_mutex_unlock(&c->mu);
        return;
    }
    c->first_pts_us = first_pts_us;
    c->elapsed_us = elapsed_us;
    c->last_apts_us = last_apts_us;
    c->fill = fill;
    c->hw_pace = hw_pace;
    if (first_pts_us != AV_NOPTS_VALUE)
        c->clock_us = first_pts_us + elapsed_us;
    else
        c->clock_us = AV_NOPTS_VALUE;
    if (ready && !c->ready) {
        c->ready = 1;
        pthread_cond_broadcast(&c->ready_cv);
    }
    pthread_mutex_unlock(&c->mu);
}

static void clock_reset(AudioClock *c)
{
    pthread_mutex_lock(&c->mu);
    c->epoch++;
    c->ready = 0;
    c->first_pts_us = AV_NOPTS_VALUE;
    c->elapsed_us = 0;
    c->clock_us = AV_NOPTS_VALUE;
    c->last_apts_us = AV_NOPTS_VALUE;
    pthread_cond_broadcast(&c->ready_cv);
    pthread_mutex_unlock(&c->mu);
}

static unsigned clock_epoch_now(AudioClock *c)
{
    unsigned e;

    pthread_mutex_lock(&c->mu);
    e = c->epoch;
    pthread_mutex_unlock(&c->mu);
    return e;
}

static int64_t clock_read(AudioClock *c, int64_t *apts_out, int *fill_out)
{
    int64_t clk;
    pthread_mutex_lock(&c->mu);
    clk = c->clock_us;
    if (apts_out)
        *apts_out = c->last_apts_us;
    if (fill_out)
        *fill_out = c->fill;
    pthread_mutex_unlock(&c->mu);
    return clk;
}

static int clock_is_ready(AudioClock *c)
{
    int r;

    pthread_mutex_lock(&c->mu);
    r = c->ready;
    pthread_mutex_unlock(&c->mu);
    return r;
}

static void mraudio_note_fill(MrAudio *a)
{
    if (a->fill < 0)
        return;
    if (!a->fill_n || a->fill < a->fill_min)
        a->fill_min = a->fill;
    if (a->fill > a->fill_max)
        a->fill_max = a->fill;
    a->fill_sum += a->fill;
    a->fill_n++;

    if (a->playing && a->fill == 0) {
        if (!a->in_underrun) {
            a->in_underrun = 1;
            a->underruns++;
            a->underrun_start = av_gettime_relative();
        }
    } else if (a->in_underrun && a->fill > 0) {
        a->underrun_us += av_gettime_relative() - a->underrun_start;
        a->in_underrun = 0;
    }
}

static int mraudio_poll(MrAudio *a)
{
    char buf[128];
    int fd, rptr = -1, wptr = -1, fill = -1, comp = -1;
    ssize_t n;
    fd = open(MRAUDIO_DEV, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    if (sscanf(buf, "rptr: %d, wptr: %d, len: %d, comp: %d",
               &rptr, &wptr, &fill, &comp) != 4)
        return -1;
    if (rptr < 0)
        return -1;
    a->rptr = rptr;
    a->wptr = wptr;
    a->fill = fill;
    a->comp = comp;
    a->polls++;
    snprintf(a->last_line, sizeof(a->last_line), "%s", buf);
    mraudio_note_fill(a);
    return 0;
}

static int64_t mraudio_submitted_media(const MrAudio *a)
{
    int64_t s = a->bytes_written - a->prime_bytes - a->bytes_origin;
    return s > 0 ? s : 0;
}

static int64_t mraudio_consumed(const MrAudio *a)
{
    int64_t c = mraudio_submitted_media(a) - (a->fill > 0 ? a->fill : 0);
    return c > 0 ? c : 0;
}

static int64_t mraudio_elapsed_us(const MrAudio *a)
{
    return av_rescale(mraudio_consumed(a), 1000000, BYTES_PER_SEC);
}

static int mraudio_open(MrAudio *a)
{
    memset(a, 0, sizeof(*a));
    a->wr_fd = -1;
    a->rptr = a->wptr = a->fill = a->comp = -1;
    a->wr_fd = open(MRAUDIO_DEV, O_WRONLY | O_CLOEXEC);
    if (a->wr_fd < 0) {
        perror("open " MRAUDIO_DEV);
        return -1;
    }
    if (mraudio_poll(a) == 0) {
        a->hw_pace = 1;
        dbg("MrAudio pacing: hardware rptr/len\n  first: %s",
            a->last_line);
    } else {
        a->hw_pace = 0;
        dbg("MrAudio rptr unavailable; wall-clock fallback.\n");
    }
    return 0;
}

static int mraudio_write_all(MrAudio *a, const uint8_t *data, int size)
{
    int done = 0;
    if (a->wr_fd < 0)
        return -1;
    while (done < size) {
        ssize_t n = write(a->wr_fd, data + done, (size_t)(size - done));
        if (n > 0) {
            if (n < size - done)
                a->short_writes++;
            done += (int)n;
            a->bytes_written += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        a->write_errors++;
        perror("write " MRAUDIO_DEV);
        return -1;
    }
    return 0;
}

static void mraudio_wait_fill(MrAudio *a, int add_bytes)
{
    if (a->hw_pace) {
        for (;;) {
            if (mraudio_poll(a) != 0) {
                a->hw_pace = 0;
                a->wall_origin = av_gettime_relative();
                break;
            }
            if (a->fill + add_bytes <= TARGET_FILL)
                break;
            {
                int excess = a->fill + add_bytes - TARGET_FILL;
                int64_t us = ((int64_t)excess * 1000000LL) / BYTES_PER_SEC;
                if (us < 1000)
                    us = 1000;
                if (us > 20000)
                    us = 20000;
                av_usleep((unsigned)us);
            }
        }
        return;
    }
    if (!a->wall_origin)
        a->wall_origin = av_gettime_relative();
    {
        int64_t media = mraudio_submitted_media(a);
        int64_t due = a->wall_origin + (media * 1000000LL) / BYTES_PER_SEC;
        int64_t now = av_gettime_relative();
        if (due > now)
            av_usleep((unsigned)(due - now));
    }
}

static int mraudio_prime(MrAudio *a)
{
    uint8_t silence[PRIME_BYTES];
    memset(silence, 0, sizeof(silence));
    dbg("Priming FPGA got_first with %d silence bytes.\n", PRIME_BYTES);
    if (mraudio_write_all(a, silence, PRIME_BYTES) < 0)
        return -1;
    a->prime_bytes = PRIME_BYTES;
    av_usleep(5000);
    if (a->hw_pace)
        mraudio_poll(a);
    return 0;
}

static void mraudio_drain(MrAudio *a)
{
    int64_t t0 = av_gettime_relative();
    if (a->in_underrun) {
        a->underrun_us += av_gettime_relative() - a->underrun_start;
        a->in_underrun = 0;
    }
    if (!a->hw_pace) {
        av_usleep(200000);
        return;
    }
    while (av_gettime_relative() - t0 < 3000000) {
        if (mraudio_poll(a) != 0)
            break;
        if (a->fill <= WRITE_CHUNK)
            break;
        av_usleep(20000);
    }
}

static void mraudio_close(MrAudio *a)
{
    if (a->wr_fd >= 0) {
        close(a->wr_fd);
        a->wr_fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Shared player                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    DVD_VIDEO_UNKNOWN = 0,
    DVD_VIDEO_PAL,
    DVD_VIDEO_NTSC
} DvdVideoStd;

struct Player {
    FBPair *fb;
    PktQ aq, vq;
    AudioClock clock;
    AVRational atb, vtb, fps, avg_fr, r_fr;
    DvdVideoStd dvd_std;
    int video_w, video_h;
    int ai, vi;
    int fail;
    int audio_started, video_started;
    AVCodecParameters *acp, *vcp;
    MrAudio mr_stats;
    int64_t audio_cpu_us, video_cpu_us, demux_cpu_us;

    unsigned long audio_packets, audio_frames;
    int64_t src_samples, out_samples;
    int audio_disc, audio_missing_pts;
    int live_underruns;
    int64_t last_audio_pts_us;

    int rendered, frames_a, frames_b, flips;
    int frames_late, late_40, late_80, video_disc, preroll_decoded;
    int video_decoded, stale_dropped;
    int initial_skip_req, initial_skip_left, initial_video_skipped;
    int video_advance_ms;
    /* OSD A/V Sync trim (ms). +N = video later vs audio; -N = video earlier.
     * 0 ms == existing --video-advance-ms baseline. MrAudio is not touched. */
    volatile int osd_av_trim_ms;
    int fpga_v1_caps;
    int fpga_yuv420;
    int yuv_meta_logged;
    int fpga_src_std;
    int stale_run, stale_run_max;
    unsigned long stale_n;
    int64_t stale_off_sum, stale_off_min, stale_off_max;
    int64_t offset_sum, last_offset, offset_max_pos, offset_max_neg;
    int offset_n;
    int64_t first_offsets[OFFSET_LOG_N], last_offsets[OFFSET_LOG_N];
    int first_off_n, last_off_n;
    int64_t raw_offset_sum, last_raw_offset, raw_offset_max_pos, raw_offset_max_neg;
    int raw_offset_n;
    int64_t first_raw_offsets[OFFSET_LOG_N], last_raw_offsets[OFFSET_LOG_N];
    int first_raw_off_n, last_raw_off_n;
    int64_t last_video_pts_us;
    int64_t first_genuine_pts, timeline_pts, assigned_pts;
    int genuine_pts_count, interpolated_count, fallback_frames;
    int64_t wait_us, vsync_us, convert_us;
    int displayed;
    int64_t present_vbl;

    /* display_buf ack diagnostics only — never used for scheduling */
    int64_t last_mbox_wall_us;
    int64_t last_ack_wall_us;
    int64_t last_ack_interval_us;
    int64_t last_mbox_to_ack_us;
    unsigned long ack_n;
    unsigned long ack_interval_n;
    int64_t ack_interval_sum;
    int64_t ack_interval_min;
    int64_t ack_interval_max;
    unsigned long ack_normal;
    unsigned long ack_miss1;
    unsigned long ack_miss2;
    unsigned long ack_miss_gt2;
    int64_t ack_missed_periods;
    unsigned long mbox_to_ack_n;
    int64_t mbox_to_ack_sum;
    int64_t mbox_to_ack_min;
    int64_t mbox_to_ack_max;
    unsigned long mbox_to_ack_0_1t;
    unsigned long mbox_to_ack_1_2t;
    unsigned long mbox_to_ack_2_3t;
    unsigned long mbox_to_ack_gt3t;
    int64_t diag_period_us;

    /*
     * Critical-path diagnostics only (presented frames). decode_us =
     * mailbox-return → receive_frame. cycle_us is present (ACK wait through
     * mailbox). mbox_cycle is previous mailbox → this mailbox. None of
     * these fields are used to schedule.
     */
    int64_t last_present_end_us;
    int64_t cur_decode_us;
    int sws_cpu_ok;

    unsigned long dec_n;
    int64_t dec_sum, dec_min, dec_max;
    unsigned long dec_gt10, dec_gt20, dec_gt30, dec_gt40;

    unsigned long ackw_n, ackw_instant;
    int64_t ackw_sum, ackw_min, ackw_max;
    unsigned long ackw_gt5, ackw_gt10, ackw_gt20, ackw_gt30, ackw_gt40;
    unsigned long ackw_gt1t, ackw_gt2t, ackw_gt3t, ackw_gt200;
    unsigned long ack_reissue_total;
    unsigned long ack_wait_reissued;
    unsigned long ack_late_n;
    unsigned long ack_recovered_late_n;
    int last_ack_want;
    int last_ack_valid_buf;
    int last_ack_valid_ok;
    uint64_t last_ack_raw;
    int last_ack_magic_ok;
    unsigned last_ack_reissues;
    int64_t last_ack_T_us;

    unsigned long sws_n;
    int64_t sws_sum, sws_min, sws_max;
    unsigned long sws_gt20, sws_gt25, sws_gt30, sws_gt35, sws_gt40, sws_gt50;

    unsigned long mbox_cyc_n;
    int64_t mbox_cyc_sum, mbox_cyc_min, mbox_cyc_max;

    unsigned long sws_cpu_n;
    int64_t sws_cpu_sum, sws_cpu_min, sws_cpu_max;
    int64_t preempt_sum, preempt_min, preempt_max;
    unsigned long preempt_gt1, preempt_gt2, preempt_gt5, preempt_gt10;

    unsigned long a2m_n;
    int64_t a2m_sum, a2m_min, a2m_max;
    unsigned long a2m_gt20, a2m_gt30, a2m_gt35, a2m_gt40, a2m_gt50;

    unsigned long c2m_n;
    int64_t c2m_sum, c2m_min, c2m_max;

    unsigned long cyc_n;
    int64_t cyc_sum, cyc_min, cyc_max;
    unsigned long cyc_gt30, cyc_gt35, cyc_gt40, cyc_gt50, cyc_gt80;

    struct {
        int valid;
        int frame;
        int64_t vpts_us;
        int64_t av_off_us;
        int64_t ack_wait_us;
        int ack_instant;
        int ack_waited;
        int64_t decode_us;
        int64_t sws_wall_us;
        int64_t sws_cpu_us;
        int64_t preempt_us;
        int64_t ack_to_mbox_us;
        int64_t conv_to_mbox_us;
        int64_t cycle_us;
    } last_path;

    struct {
        unsigned miss_n;
        int frame;
        int64_t vpts_us;
        int64_t av_off_us;
        int64_t ack_iv_us;
        int64_t ack_wait_us;
        int ack_instant;
        int64_t decode_us;
        int64_t sws_wall_us;
        int64_t sws_cpu_us;
        int64_t preempt_us;
        int64_t ack_to_mbox_us;
        int64_t conv_to_mbox_us;
        int64_t mbox_to_ack_us;
        int64_t cycle_us;
    } misses[MISS_LOG_CAP];
    unsigned miss_log_n, miss_log_i, miss_total;

    int ncpu_onln, ncpu_conf;
    unsigned cpu_aff_mask;
    int sched_video_cpu, sched_audio_cpu, sched_demux_cpu, sched_present_cpu;
    int sched_input_cpu;
    int input_started;
    unsigned long ctrl_events;
    int64_t input_cpu_us;

    /* DVD menu navigation. Input thread posts; demux/dvdnav thread executes. */
    struct {
        pthread_mutex_t mu;
        NavCmd cmds[NAVQ_CAP];
        int head, tail, count;
        int inited;
    } navq;
    struct {
        pthread_mutex_t mu;
        int visible;
        int button;
        int btn_ns;
        int sx, sy, ex, ey;
        uint32_t palette;
        uint32_t palette_act;
        unsigned pci_lbn;
        int hli_ss;
        int activated;
        unsigned gen;
        int redraw;
        int in_menu;
        int still;
        int inited;
    } hl;
    struct {
        pthread_mutex_t mu;
        int inited;
        int valid;
        unsigned gen;
        int x, y, w, h;
        uint8_t color[4];
        uint8_t alpha[4];
        uint8_t *idx;
        uint8_t *acc;
        int acc_size;
        int pes_id;
        int logical;
        int physical_wide;
        int physical_letterbox;
        uint32_t clut[16];
        uint32_t clut_bgr0[16];
        int clut_valid;
        unsigned seen_nb_streams;
        int decoder_logged;
        int streams_logged;
        int tile_dirty;
        int tile_valid;
        int tile_n;
        int tile_cap;
        int tile_x, tile_y, tile_w, tile_h;
        struct {
            uint16_t x, y;
            uint8_t a8;
            uint8_t pad;
            uint32_t bgr;
        } *tile_px;
    } spu;
    /* Movie (title-domain) SPU. Separate from menu HLI/SPU state. */
    struct {
        pthread_mutex_t mu;
        int inited;
        int valid;
        int forced;
        int shown;
        int visible_now;
        int prefer_done;
        int64_t packet_pts_us;
        int64_t from_us;
        int64_t until_us;
        int x, y, w, h;
        int top_off, bot_off;
        uint8_t color[4];
        uint8_t alpha[4];
        uint8_t *idx;
        uint8_t *acc;
        int acc_size;
        int64_t acc_pts_us;
        int pes_id;
        int logical;
        int physical_wide;
        int physical_letterbox;
        int physical_pan_scan;
        int chosen_physical;
        unsigned spu_seq;
        unsigned last_spu_id;
        int saw_chg_colcon;
        int evt_n;
        int chg_n;
        MsubEvt evt[MSUB_EVT_MAX];
        MsubChg chg[MSUB_CHG_MAX];
        unsigned long spu_fragments;
        unsigned long complete_spus;
        unsigned long decoded_spus;
        unsigned long displayed_spus;
        unsigned long malformed_spus;
        unsigned long chg_colcon_spus;
        unsigned long chg_colcon_events;
        unsigned long malformed_chg_colcon;
        uint64_t bbox_w_sum, bbox_h_sum;
        unsigned bbox_n, bbox_w_max, bbox_h_max;
    } msub;
    struct {
        int last_active;
        int inited;
        int64_t *act_blend;
        int64_t *act_sws;
        int64_t *act_ack;
        int64_t *act_cyc;
        int64_t *act_sws_sub;
        int64_t *inact_sws;
        int64_t *inact_ack;
        int64_t *inact_cyc;
        int act_blend_n, act_sws_n, act_ack_n, act_cyc_n, act_combo_n;
        int inact_sws_n, inact_ack_n, inact_cyc_n;
        int64_t act_blend_sum, act_blend_max;
        int64_t act_sws_sum, act_ack_sum, act_cyc_sum, act_combo_sum;
        int64_t inact_sws_sum, inact_ack_sum, inact_cyc_sum;
        unsigned long act_stale, inact_stale;
        unsigned long act_miss, inact_miss;
        unsigned long act_frames, inact_frames;
    } subperf;
    struct {
        unsigned long menu_frames;
        int64_t sws_sum;
        int64_t ov_sum;
        int64_t ov_max;
        unsigned long still_redraws;
        int64_t still_sum;
        uint64_t px_sum;
        uint64_t bbox_sum;
        unsigned bbox_max;
    } spu_perf;
    unsigned nav_gen;
    unsigned codec_gen;
    unsigned frames_this_nav_gen;
    int still_drain_req;
    int still_drain_done;
    unsigned still_drain_gen;
    int soft_decode_trace;
    unsigned flush_n;
    int in_menu;
    int still_active;
    int menu_still_drop;
    int video_reset_req;
    int audio_reset_req;
    int audio_switch_req;
    int current_audio_logical;
    int audio_pending_logical;
    int audio_follow_logical;
    int audio_follow_physical;
    int audio_follow_fmt;
    AVFormatContext *avf;
    int demux_reopen_req;
    int soft_nav_log;
    unsigned soft_nav_gen;
    int soft_log_pkt;
    int soft_log_decode;
    int soft_log_yuv;
    unsigned long menu_redraws;
    unsigned long nav_flush_pkts;
    unsigned long nav_flush_yuv;

    /* Title play/pause. Input posts NAVCMD_PLAY_PAUSE; audio owns drain. */
    struct {
        pthread_mutex_t mu;
        pthread_cond_t cv;
        int inited;
        int mode; /* 0 off, 1 pending drain, 2 held */
        unsigned events;
        int64_t req_us;
        int64_t drain_us;
        int fill_at_req;
        int fill_at_held;
        int64_t consumed_at_req;
        int64_t consumed_at_held;
        int64_t clock_at_req;
        int64_t clock_at_held;
        int64_t clock_at_resume;
        int64_t resume_us;
        int64_t first_vpts_resume;
        int64_t first_audio_write_resume;
        int64_t av_off_at_req;
        int64_t av_off_at_held;
        int64_t av_off_at_resume;
        int stale_at_req;
        int stale_at_resume;
        int aq_at_held;
        int vq_at_held;
        int yuv_at_held;
        int need_resume_vpts;
        int need_resume_audio;
        unsigned wake_gen;
        int st_demux, st_audio, st_video, st_present;
        int stuck_logged;
    } pause;

    /* --uncapped-video-benchmark only. Default playback never reads these. */
    int uncapped_bench;
    int bench_active_buf;
    int bench_target_buf;
    int bench_bufs_ok;
    int bench_display_end;
    uint32_t bench_mbox0_start;
    uint32_t bench_mbox0_end;
    unsigned long bench_audio_discarded;
    unsigned long mailbox_writes;
    int bench_decoded;
    int bench_converted;
    int bench_decode_errors;
    int64_t bench_t0_us;
    int64_t bench_t1_us;
    int64_t bench_last_mark_us;
    int64_t bench_video_cpu0;
    int64_t bench_video_cpu_us;
    unsigned long bench_dec_n;
    int64_t bench_dec_sum, bench_dec_min, bench_dec_max;
    unsigned long bench_sws_n;
    int64_t bench_sws_sum, bench_sws_min, bench_sws_max;
    unsigned long bench_sws_cpu_n;
    int64_t bench_sws_cpu_sum, bench_sws_cpu_min, bench_sws_cpu_max;
    unsigned long bench_combo_n;
    int64_t bench_combo_sum, bench_combo_min, bench_combo_max;

    /* --buffered-video / --buffered-yuv-video. Default playback never
     * reads these. */
    int buffered_video;
    int buffered_yuv;
    int perf_present_no_convert;
    int iso_warm_presents;
    int iso_started;
    int64_t iso_t0_us;
    int iso_decoded0;
    int iso_stale0;
    int iso_n;
    int iso_ack_n;
    int64_t *iso_ack;
    int64_t *iso_cyc;
    int64_t iso_ack_sum;
    int64_t iso_cyc_sum;
    unsigned long iso_ack_gt20, iso_ack_gt30, iso_ack_gt40;
    int phase_decode;
    int phase_inited;
    pthread_mutex_t phase_mu;
    pthread_cond_t phase_cv;
    int sws_busy;
    int producer_in_decode;
    int64_t phase_t0_us;
    unsigned long phase_overlap_n;
    unsigned long phase_wait_n;
    unsigned long phase_bypass_low;
    unsigned long phase_bypass_vq;
    int64_t *phase_sws;
    int64_t *phase_ack;
    int64_t *phase_cyc;
    int phase_sws_n, phase_ack_n, phase_cyc_n;
    int64_t phase_sws_sum, phase_ack_sum, phase_cyc_sum;
    int64_t *yuv_copy;
    int yuv_copy_n;
    int64_t yuv_copy_sum;
    int64_t yuv_copy_max;
    int present_started;
    int buf_playing;
    VidRing vring;
    YuvRing yuvring;
    pthread_mutex_t prefill_mu;
    pthread_cond_t prefill_cv;
    int prefill_released;
    int prefill_req;
    int prefill_got;
    int64_t prefill_t0_us;
    int64_t prefill_t1_us;
    const char *prefill_reason;
    uint8_t *pcm_hold;
    int pcm_hold_len;
    int pcm_hold_cap;
    int64_t pcm_hold_discarded;
    int pcm_hold_at_release;
    int pcm_hold_used;
    int64_t first_audio_pts_us;
    int buf_mraudio_started;
    int64_t present_cpu_us;
    int64_t prod_last_end_us;
    unsigned long prod_dec_n;
    int64_t prod_dec_sum, prod_dec_min, prod_dec_max;
    unsigned long prod_sws_n;
    int64_t prod_sws_sum, prod_sws_min, prod_sws_max;
    unsigned long prod_sws_cpu_n;
    int64_t prod_sws_cpu_sum, prod_sws_cpu_min, prod_sws_cpu_max;
    int64_t prod_t0_us, prod_t1_us;
    int prod_enqueued;
    unsigned long memcpy_n;
    int64_t memcpy_sum, memcpy_min, memcpy_max;
    unsigned long memcpy_cpu_n;
    int64_t memcpy_cpu_sum, memcpy_cpu_min, memcpy_cpu_max;
    unsigned long memcpy_gt20, memcpy_gt25, memcpy_gt30, memcpy_gt35;
    unsigned long memcpy_gt40, memcpy_gt50;
};

static int player_buffered(const Player *p)
{
    return p->buffered_video || p->buffered_yuv;
}

static int buffered_queue_count(Player *p)
{
    if (p->buffered_yuv)
        return yuvring_count(&p->yuvring);
    return vidring_count(&p->vring);
}

static void player_abort(Player *p)
{
    p->fail = 1;
    pause_cancel(p);
    pktq_quit(&p->aq);
    pktq_quit(&p->vq);
    if (player_buffered(p)) {
        vidring_quit(&p->vring);
        yuvring_quit(&p->yuvring);
        pthread_mutex_lock(&p->prefill_mu);
        p->prefill_released = 1;
        if (!p->prefill_reason)
            p->prefill_reason = "player abort";
        pthread_cond_broadcast(&p->prefill_cv);
        pthread_mutex_unlock(&p->prefill_mu);
        pthread_mutex_lock(&p->clock.mu);
        pthread_cond_broadcast(&p->clock.ready_cv);
        pthread_mutex_unlock(&p->clock.mu);
        pthread_mutex_lock(&p->aq.mu);
        pthread_cond_broadcast(&p->aq.not_empty);
        pthread_cond_broadcast(&p->aq.not_full);
        pthread_mutex_unlock(&p->aq.mu);
        pthread_mutex_lock(&p->vq.mu);
        pthread_cond_broadcast(&p->vq.not_empty);
        pthread_cond_broadcast(&p->vq.not_full);
        pthread_mutex_unlock(&p->vq.mu);
    }
    if (p->phase_inited) {
        pthread_mutex_lock(&p->phase_mu);
        pthread_cond_broadcast(&p->phase_cv);
        pthread_mutex_unlock(&p->phase_mu);
    }
}

static unsigned player_nav_gen(const Player *p)
{
    return __atomic_load_n(&p->nav_gen, __ATOMIC_SEQ_CST);
}

static unsigned player_codec_gen(const Player *p)
{
    return __atomic_load_n(&p->codec_gen, __ATOMIC_SEQ_CST);
}

static void dvdio_note_mpeg_return(DVDIO *d, int n)
{
    if (!d || !d->soft_hop_active || n <= 0)
        return;
    d->post_soft_bytes += n;
    if (!d->post_soft_mpeg_ret) {
        d->post_soft_mpeg_ret = 1;
        dbg("DVD MENU: first MPEG packet returned after SOFT HOP  "
            "bytes=%d  preserved_total=%d  gen=%u\n",
            n, d->post_soft_bytes,
            d->player ? player_nav_gen(d->player) : 0);
    }
}

static void navq_init(Player *p)
{
    memset(&p->navq, 0, sizeof(p->navq));
    pthread_mutex_init(&p->navq.mu, NULL);
    p->navq.inited = 1;
    memset(&p->hl, 0, sizeof(p->hl));
    pthread_mutex_init(&p->hl.mu, NULL);
    p->hl.inited = 1;
    memset(&p->spu, 0, sizeof(p->spu));
    pthread_mutex_init(&p->spu.mu, NULL);
    p->spu.inited = 1;
    p->spu.pes_id = -1;
    p->spu.logical = -1;
    p->spu.physical_wide = -1;
    p->spu.physical_letterbox = -1;
    memset(&p->msub, 0, sizeof(p->msub));
    pthread_mutex_init(&p->msub.mu, NULL);
    p->msub.inited = 1;
    p->msub.pes_id = -1;
    p->msub.logical = -1;
    p->msub.physical_wide = -1;
    p->msub.physical_letterbox = -1;
    p->msub.physical_pan_scan = -1;
    p->msub.chosen_physical = -1;
    p->msub.packet_pts_us = AV_NOPTS_VALUE;
    p->msub.from_us = AV_NOPTS_VALUE;
    p->msub.until_us = AV_NOPTS_VALUE;
    p->msub.acc_pts_us = AV_NOPTS_VALUE;
    p->nav_gen = 1;
    p->codec_gen = 1;
    memset(&p->pause, 0, sizeof(p->pause));
    pthread_mutex_init(&p->pause.mu, NULL);
    pthread_cond_init(&p->pause.cv, NULL);
    p->pause.inited = 1;
    p->pause.first_vpts_resume = AV_NOPTS_VALUE;
    p->pause.clock_at_req = AV_NOPTS_VALUE;
    p->pause.clock_at_held = AV_NOPTS_VALUE;
    p->pause.clock_at_resume = AV_NOPTS_VALUE;
    p->pause.resume_us = AV_NOPTS_VALUE;
    p->pause.first_audio_write_resume = AV_NOPTS_VALUE;
    p->pause.av_off_at_req = AV_NOPTS_VALUE;
    p->pause.av_off_at_held = AV_NOPTS_VALUE;
    p->pause.av_off_at_resume = AV_NOPTS_VALUE;
}

static void navq_destroy(Player *p)
{
    if (p->navq.inited) {
        pthread_mutex_destroy(&p->navq.mu);
        p->navq.inited = 0;
    }
    if (p->hl.inited) {
        pthread_mutex_destroy(&p->hl.mu);
        p->hl.inited = 0;
    }
    if (p->spu.inited) {
        pthread_mutex_destroy(&p->spu.mu);
        p->spu.inited = 0;
    }
    free(p->spu.idx);
    p->spu.idx = NULL;
    free(p->spu.acc);
    p->spu.acc = NULL;
    free(p->spu.tile_px);
    p->spu.tile_px = NULL;
    if (p->msub.inited) {
        pthread_mutex_destroy(&p->msub.mu);
        p->msub.inited = 0;
    }
    free(p->msub.idx);
    p->msub.idx = NULL;
    free(p->msub.acc);
    p->msub.acc = NULL;
    if (p->pause.inited) {
        pthread_mutex_destroy(&p->pause.mu);
        pthread_cond_destroy(&p->pause.cv);
        p->pause.inited = 0;
    }
}

static void navq_post(Player *p, NavCmd cmd)
{
    if (!p || !p->navq.inited || cmd == NAVCMD_NONE)
        return;
    pthread_mutex_lock(&p->navq.mu);
    if (p->navq.count < NAVQ_CAP) {
        p->navq.cmds[p->navq.tail] = cmd;
        p->navq.tail = (p->navq.tail + 1) % NAVQ_CAP;
        p->navq.count++;
    }
    pthread_mutex_unlock(&p->navq.mu);
    /* Wake demux immediately — do not wait for a media-queue timeout. */
    if (p->pause.inited) {
        pthread_mutex_lock(&p->pause.mu);
        p->pause.wake_gen++;
        pthread_cond_broadcast(&p->pause.cv);
        pthread_mutex_unlock(&p->pause.mu);
    }
}

static NavCmd navq_pop(Player *p)
{
    NavCmd cmd = NAVCMD_NONE;

    if (!p || !p->navq.inited)
        return NAVCMD_NONE;
    pthread_mutex_lock(&p->navq.mu);
    if (p->navq.count > 0) {
        cmd = p->navq.cmds[p->navq.head];
        p->navq.head = (p->navq.head + 1) % NAVQ_CAP;
        p->navq.count--;
    }
    pthread_mutex_unlock(&p->navq.mu);
    return cmd;
}

static int navq_count(Player *p)
{
    int n;

    if (!p || !p->navq.inited)
        return 0;
    pthread_mutex_lock(&p->navq.mu);
    n = p->navq.count;
    pthread_mutex_unlock(&p->navq.mu);
    return n;
}

enum {
    PAUSE_OFF = 0,
    PAUSE_PENDING = 1,
    PAUSE_HELD = 2
};

static void pause_wake(Player *p)
{
    if (!p || !p->pause.inited)
        return;
    pthread_mutex_lock(&p->pause.mu);
    p->pause.wake_gen++;
    pthread_cond_broadcast(&p->pause.cv);
    pthread_mutex_unlock(&p->pause.mu);
    if (p->aq.pkts) {
        pthread_mutex_lock(&p->aq.mu);
        pthread_cond_broadcast(&p->aq.not_empty);
        pthread_cond_broadcast(&p->aq.not_full);
        pthread_mutex_unlock(&p->aq.mu);
    }
    if (p->vq.pkts) {
        pthread_mutex_lock(&p->vq.mu);
        pthread_cond_broadcast(&p->vq.not_empty);
        pthread_cond_broadcast(&p->vq.not_full);
        pthread_mutex_unlock(&p->vq.mu);
    }
    if (p->buffered_yuv && p->yuvring.inited) {
        pthread_mutex_lock(&p->yuvring.mu);
        pthread_cond_broadcast(&p->yuvring.not_empty);
        pthread_cond_broadcast(&p->yuvring.not_full);
        pthread_mutex_unlock(&p->yuvring.mu);
    }
    if (p->buffered_video && p->vring.inited) {
        pthread_mutex_lock(&p->vring.mu);
        pthread_cond_broadcast(&p->vring.not_empty);
        pthread_cond_broadcast(&p->vring.not_full);
        pthread_mutex_unlock(&p->vring.mu);
    }
}

static int pause_mode(Player *p)
{
    int m;

    if (!p || !p->pause.inited)
        return PAUSE_OFF;
    pthread_mutex_lock(&p->pause.mu);
    m = p->pause.mode;
    pthread_mutex_unlock(&p->pause.mu);
    return m;
}

static int pause_should_hold_audio(Player *p)
{
    int m = pause_mode(p);
    return m == PAUSE_PENDING || m == PAUSE_HELD;
}

static int pause_is_held(Player *p)
{
    return pause_mode(p) == PAUSE_HELD;
}

static void pause_wait_unheld(Player *p)
{
    if (!p || !p->pause.inited)
        return;
    pthread_mutex_lock(&p->pause.mu);
    while (p->pause.mode == PAUSE_HELD && !p->fail && !g_interrupt)
        pthread_cond_wait(&p->pause.cv, &p->pause.mu);
    pthread_mutex_unlock(&p->pause.mu);
}

static int pause_wait_control(Player *p)
{
    unsigned gen;

    if (!p || !p->pause.inited)
        return 0;
    pause_thr_set(&p->pause.st_demux, THR_PAUSE);
    pthread_mutex_lock(&p->pause.mu);
    while (p->pause.mode != PAUSE_OFF && !p->fail && !g_interrupt) {
        gen = p->pause.wake_gen;
        pthread_mutex_unlock(&p->pause.mu);
        if (navq_count(p) > 0) {
            pause_thr_set(&p->pause.st_demux, THR_RUN);
            return 1;
        }
        pthread_mutex_lock(&p->pause.mu);
        if (p->pause.mode == PAUSE_OFF || p->fail || g_interrupt)
            break;
        if (p->pause.wake_gen != gen)
            continue;
        pthread_cond_wait(&p->pause.cv, &p->pause.mu);
    }
    pthread_mutex_unlock(&p->pause.mu);
    pause_thr_set(&p->pause.st_demux, THR_RUN);
    return 0;
}

static void pause_debug_threads(Player *p, const char *why)
{
    if (!p || !g_debug_stats)
        return;
    fprintf(stderr,
            "PAUSE thread state%s%s:\n"
            "  demux=%s\n"
            "  audio=%s\n"
            "  video producer=%s\n"
            "  presenter=%s\n"
            "  aq count=%d\n"
            "  vq count=%d\n"
            "  yuv count=%d\n"
            "  pending nav commands=%d\n",
            why && why[0] ? " (" : "",
            why && why[0] ? why : "",
            pause_thr_name(pause_thr_get(&p->pause.st_demux)),
            pause_thr_name(pause_thr_get(&p->pause.st_audio)),
            pause_thr_name(pause_thr_get(&p->pause.st_video)),
            pause_thr_name(pause_thr_get(&p->pause.st_present)),
            pktq_count(&p->aq),
            pktq_count(&p->vq),
            (p->buffered_yuv && p->yuvring.inited)
                ? yuvring_count(&p->yuvring) : 0,
            navq_count(p));
}

/* 1 = pushed, 0 = not pushed (quit, interrupt, or pending nav). */
static int player_pktq_push(Player *p, PktQ *q, AVPacket *src)
{
    int64_t t0 = 0;
    const char *qname;

    if (!p || !q || !src)
        return 0;
    qname = (q == &p->aq) ? "audio" : (q == &p->vq) ? "video" : "packet";
    pthread_mutex_lock(&q->mu);
    while (q->count >= q->cap && !q->quit && !g_interrupt) {
        if (!t0) {
            t0 = av_gettime_relative();
            q->full_n++;
            pause_thr_set(&p->pause.st_demux, THR_QFULL);
        }
        if (navq_count(p) > 0) {
            if (t0)
                q->block_us += av_gettime_relative() - t0;
            pthread_mutex_unlock(&q->mu);
            pause_thr_set(&p->pause.st_demux, THR_RUN);
            return 0;
        }
        if (pause_should_hold_audio(p)) {
            if (g_debug_stats && !p->pause.stuck_logged &&
                av_gettime_relative() - t0 >= PAUSE_STUCK_US) {
                p->pause.stuck_logged = 1;
                fprintf(stderr,
                        "DVD PAUSE: demux blocked on %s queue >500ms\n",
                        qname);
            }
            pthread_mutex_unlock(&q->mu);
            pause_wait_control(p);
            pthread_mutex_lock(&q->mu);
            continue;
        }
        pktq_wait_timeout(&q->not_full, &q->mu);
    }
    if (g_interrupt)
        q->quit = 1;
    if (t0)
        q->block_us += av_gettime_relative() - t0;
    pause_thr_set(&p->pause.st_demux, THR_RUN);
    if (q->quit) {
        pthread_mutex_unlock(&q->mu);
        return 0;
    }
    av_packet_ref(&q->pkts[q->tail], src);
    q->pkts[q->tail].opaque = src->opaque;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    q->pushed++;
    pktq_note_depth(q);
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return 1;
}

static void pause_request_toggle(Player *p)
{
    int mode;
    int fill = 0;
    int ready;
    int64_t clk;

    if (!p || !p->pause.inited)
        return;
    clk = clock_read(&p->clock, NULL, &fill);
    ready = clock_is_ready(&p->clock);
    pthread_mutex_lock(&p->pause.mu);
    mode = p->pause.mode;
    if (mode == PAUSE_OFF) {
        if (!ready || p->in_menu || p->still_active) {
            pthread_mutex_unlock(&p->pause.mu);
            if (p->in_menu || p->still_active)
                fprintf(stderr, "DVD PAUSE: unavailable in menu\n");
            else
                fprintf(stderr, "DVD PAUSE: unavailable until playback\n");
            return;
        }
        p->pause.mode = PAUSE_PENDING;
        p->pause.events++;
        p->pause.req_us = av_gettime_relative();
        p->pause.clock_at_req = clk;
        p->pause.fill_at_req = fill;
        p->pause.stale_at_req = p->stale_dropped;
        p->pause.av_off_at_req = p->last_offset;
        p->pause.av_off_at_held = AV_NOPTS_VALUE;
        p->pause.av_off_at_resume = AV_NOPTS_VALUE;
        p->pause.need_resume_vpts = 0;
        p->pause.need_resume_audio = 0;
        p->pause.first_vpts_resume = AV_NOPTS_VALUE;
        p->pause.first_audio_write_resume = AV_NOPTS_VALUE;
        p->pause.stuck_logged = 0;
        pthread_mutex_unlock(&p->pause.mu);
        fprintf(stderr, "DVD PAUSE: pending\n");
        pause_wake(p);
        return;
    }
    p->pause.mode = PAUSE_OFF;
    p->pause.clock_at_resume = clk;
    p->pause.resume_us = av_gettime_relative();
    p->pause.stale_at_resume = p->stale_dropped;
    p->pause.need_resume_vpts = 1;
    p->pause.need_resume_audio = 1;
    pthread_mutex_unlock(&p->pause.mu);
    fprintf(stderr, "DVD PAUSE: resumed\n");
    pause_wake(p);
}

static void pause_cancel(Player *p)
{
    int mode;

    if (!p || !p->pause.inited)
        return;
    pthread_mutex_lock(&p->pause.mu);
    mode = p->pause.mode;
    p->pause.mode = PAUSE_OFF;
    p->pause.need_resume_vpts = 0;
    p->pause.need_resume_audio = 0;
    pthread_mutex_unlock(&p->pause.mu);
    if (mode != PAUSE_OFF)
        pause_wake(p);
}

static void pause_enter_held(Player *p, MrAudio *mr, int64_t drain_us)
{
    int64_t clk = clock_read(&p->clock, NULL, NULL);

    pthread_mutex_lock(&p->pause.mu);
    if (p->pause.mode == PAUSE_PENDING) {
        p->pause.mode = PAUSE_HELD;
        p->pause.drain_us = drain_us;
        p->pause.clock_at_held = clk;
        p->pause.av_off_at_held = p->last_offset;
        if (mr) {
            p->pause.consumed_at_held = mraudio_consumed(mr);
            p->pause.fill_at_held = mr->fill > 0 ? mr->fill : 0;
        }
        p->pause.aq_at_held = pktq_count(&p->aq);
        p->pause.vq_at_held = pktq_count(&p->vq);
        p->pause.yuv_at_held = (p->buffered_yuv && p->yuvring.inited)
                               ? yuvring_count(&p->yuvring) : 0;
        pthread_mutex_unlock(&p->pause.mu);
        fprintf(stderr, "DVD PAUSE: paused after %dms drain\n",
                (int)((drain_us + 500) / 1000));
        pause_wake(p);
        pause_debug_threads(p, "held");
        return;
    }
    pthread_mutex_unlock(&p->pause.mu);
}

static void pause_service_audio(Player *p, MrAudio *mr, int64_t first_pts_us,
                                unsigned clock_epoch)
{
    int64_t t0, consumed0;

    if (!pause_should_hold_audio(p))
        return;

    pause_thr_set(&p->pause.st_audio, THR_PAUSE);
    if (pause_mode(p) == PAUSE_PENDING) {
        t0 = av_gettime_relative();
        if (mr->hw_pace)
            mraudio_poll(mr);
        consumed0 = mraudio_consumed(mr);
        pthread_mutex_lock(&p->pause.mu);
        p->pause.consumed_at_req = consumed0;
        pthread_mutex_unlock(&p->pause.mu);
        mr->playing = 0;
        while (pause_mode(p) == PAUSE_PENDING && !p->fail && !g_interrupt) {
            if (p->audio_reset_req)
                break;
            if (mr->hw_pace)
                mraudio_poll(mr);
            clock_publish(&p->clock, first_pts_us, mraudio_elapsed_us(mr),
                          p->last_audio_pts_us, mr->fill, mr->hw_pace,
                          first_pts_us != AV_NOPTS_VALUE &&
                          (player_buffered(p) ? p->buf_mraudio_started : 1),
                          clock_epoch);
            if (!mr->hw_pace || mr->fill <= PAUSE_DRAIN_THRESH) {
                pause_enter_held(p, mr, av_gettime_relative() - t0);
                break;
            }
            av_usleep(mr->fill <= TARGET_FILL / 4 ? 2000 : 10000);
        }
    }

    while (pause_is_held(p) && !p->fail && !g_interrupt) {
        if (p->audio_reset_req)
            break;
        pause_wait_unheld(p);
    }
    pause_thr_set(&p->pause.st_audio, THR_RUN);
}

static void pause_note_resume_video_pts(Player *p, int64_t vpts_us)
{
    if (!p || !p->pause.inited || vpts_us == AV_NOPTS_VALUE)
        return;
    pthread_mutex_lock(&p->pause.mu);
    if (p->pause.need_resume_vpts) {
        p->pause.first_vpts_resume = vpts_us;
        p->pause.av_off_at_resume = p->last_offset;
        p->pause.need_resume_vpts = 0;
    }
    pthread_mutex_unlock(&p->pause.mu);
}

static void pause_note_resume_audio_write(Player *p)
{
    if (!p || !p->pause.inited)
        return;
    pthread_mutex_lock(&p->pause.mu);
    if (p->pause.need_resume_audio) {
        p->pause.first_audio_write_resume = av_gettime_relative();
        p->pause.need_resume_audio = 0;
    }
    pthread_mutex_unlock(&p->pause.mu);
}

static const char *dvd_menu_id_name(int32_t part)
{
    switch (part) {
    case DVD_MENU_Escape:     return "Escape";
    case DVD_MENU_Title:      return "Title";
    case DVD_MENU_Root:       return "Root";
    case DVD_MENU_Subpicture: return "Subpicture";
    case DVD_MENU_Audio:      return "Audio";
    case DVD_MENU_Angle:      return "Angle";
    case DVD_MENU_Part:       return "Part";
    default:                  return "menu";
    }
}

static const char *dvd_domain_name(DVDDomain_t domain)
{
    switch (domain) {
    case DVD_DOMAIN_FirstPlay: return "FirstPlay";
    case DVD_DOMAIN_VTSTitle:  return "VTS";
    case DVD_DOMAIN_VMGM:      return "VMGM";
    case DVD_DOMAIN_VTSMenu:   return "VTSMenu";
    default:                   return "?";
    }
}

static const char *dvdnav_event_name(int32_t event)
{
    switch (event) {
    case DVDNAV_BLOCK_OK:            return "BLOCK_OK";
    case DVDNAV_NOP:                 return "NOP";
    case DVDNAV_STILL_FRAME:         return "STILL_FRAME";
    case DVDNAV_WAIT:                return "WAIT";
    case DVDNAV_SPU_STREAM_CHANGE:   return "SPU_STREAM_CHANGE";
    case DVDNAV_SPU_CLUT_CHANGE:     return "SPU_CLUT_CHANGE";
    case DVDNAV_AUDIO_STREAM_CHANGE: return "AUDIO_STREAM_CHANGE";
    case DVDNAV_VTS_CHANGE:          return "VTS_CHANGE";
    case DVDNAV_CELL_CHANGE:         return "CELL_CHANGE";
    case DVDNAV_NAV_PACKET:          return "NAV_PACKET";
    case DVDNAV_HIGHLIGHT:           return "HIGHLIGHT";
    case DVDNAV_HOP_CHANNEL:         return "HOP_CHANNEL";
    case DVDNAV_STOP:                return "STOP";
    default:                         return "OTHER";
    }
}

static unsigned dvd_vm_bits(uint64_t insn, int start, int width)
{
    if (width <= 0 || width >= 32)
        return 0;
    return (unsigned)((insn >> (start - width + 1)) & ((1ULL << width) - 1));
}

static uint64_t dvd_vm_insn(const vm_cmd_t *cmd)
{
    int i;
    uint64_t v = 0;

    for (i = 0; i < 8; i++)
        v = (v << 8) | cmd->bytes[i];
    return v;
}

static const char *dvd_vm_linksub_name(unsigned op)
{
    static const char *const names[] = {
        "LinkNoLink", "LinkTopC", "LinkNextC", "LinkPrevC",
        "?", "LinkTopPG", "LinkNextPG", "LinkPrevPG",
        "?", "LinkTopPGC", "LinkNextPGC", "LinkPrevPGC",
        "LinkGoUpPGC", "LinkTailPGC", "?", "?",
        "RSM"
    };

    if (op < (unsigned)(sizeof(names) / sizeof(names[0])))
        return names[op];
    return "?";
}

/* Classify the authored button command: title/episode jump vs menu-internal. */
static void dvd_vm_cmd_describe(const vm_cmd_t *cmd, char *out, size_t outsz)
{
    uint64_t insn = dvd_vm_insn(cmd);
    unsigned type = dvd_vm_bits(insn, 63, 3);
    unsigned sub, linkop, title, ptt, pgc, menu, vts;
    char hex[28];

    snprintf(hex, sizeof(hex), "%02x %02x %02x %02x %02x %02x %02x %02x",
             cmd->bytes[0], cmd->bytes[1], cmd->bytes[2], cmd->bytes[3],
             cmd->bytes[4], cmd->bytes[5], cmd->bytes[6], cmd->bytes[7]);

    if (insn == 0) {
        snprintf(out, outsz, "%s | Nop  (no jump)", hex);
        return;
    }
    if (type == 1 && dvd_vm_bits(insn, 60, 1)) {
        sub = dvd_vm_bits(insn, 51, 4);
        switch (sub) {
        case 1:
            snprintf(out, outsz, "%s | Exit", hex);
            return;
        case 2:
            title = dvd_vm_bits(insn, 22, 7);
            snprintf(out, outsz, "%s | JumpTT %u  (title/episode)", hex, title);
            return;
        case 3:
            title = dvd_vm_bits(insn, 22, 7);
            snprintf(out, outsz, "%s | JumpVTS_TT %u  (title in this VTS)",
                     hex, title);
            return;
        case 5:
            title = dvd_vm_bits(insn, 22, 7);
            ptt = dvd_vm_bits(insn, 41, 10);
            snprintf(out, outsz, "%s | JumpVTS_PTT %u:%u  (title:chapter)",
                     hex, title, ptt);
            return;
        case 6:
            switch (dvd_vm_bits(insn, 23, 2)) {
            case 0:
                snprintf(out, outsz, "%s | JumpSS FP  (menu)", hex);
                return;
            case 1:
                menu = dvd_vm_bits(insn, 19, 4);
                snprintf(out, outsz, "%s | JumpSS VMGM menu=%u  (menu)",
                         hex, menu);
                return;
            case 2:
                vts = dvd_vm_bits(insn, 30, 7);
                title = dvd_vm_bits(insn, 38, 7);
                menu = dvd_vm_bits(insn, 19, 4);
                snprintf(out, outsz,
                         "%s | JumpSS VTSM vts=%u title=%u menu=%u  (menu)",
                         hex, vts, title, menu);
                return;
            case 3:
                pgc = dvd_vm_bits(insn, 46, 15);
                snprintf(out, outsz, "%s | JumpSS VMGM pgc=%u  (menu)",
                         hex, pgc);
                return;
            }
            break;
        case 8:
            snprintf(out, outsz, "%s | CallSS  (call menu, RSM later)", hex);
            return;
        default:
            snprintf(out, outsz, "%s | Jump/Call sub=%u", hex, sub);
            return;
        }
    }
    if (type == 1 && !dvd_vm_bits(insn, 60, 1)) {
        sub = dvd_vm_bits(insn, 51, 4);
        switch (sub) {
        case 1:
            linkop = dvd_vm_bits(insn, 7, 8);
            snprintf(out, outsz, "%s | %s (button %u)  (menu-internal)",
                     hex, dvd_vm_linksub_name(linkop),
                     dvd_vm_bits(insn, 15, 6));
            return;
        case 4:
            snprintf(out, outsz, "%s | LinkPGCN %u  (PGC link, often same menu)",
                     hex, dvd_vm_bits(insn, 14, 15));
            return;
        case 5:
            snprintf(out, outsz, "%s | LinkPTT %u  (chapter in current title)",
                     hex, dvd_vm_bits(insn, 9, 10));
            return;
        case 6:
            snprintf(out, outsz, "%s | LinkPGN %u  (program in current PGC)",
                     hex, dvd_vm_bits(insn, 6, 7));
            return;
        case 7:
            snprintf(out, outsz, "%s | LinkCN %u  (cell in current PGC)",
                     hex, dvd_vm_bits(insn, 7, 8));
            return;
        default:
            snprintf(out, outsz, "%s | Link sub=%u", hex, sub);
            return;
        }
    }
    if (type == 0 && dvd_vm_bits(insn, 51, 4) == 0) {
        snprintf(out, outsz, "%s | Nop  (no jump)", hex);
        return;
    }
    snprintf(out, outsz, "%s | VM type=%u (not a bare jump; may be Set/If)",
             hex, type);
}

static int dvdio_detect_menu(DVDIO *d)
{
    int32_t title = 0, part = 0;

    if (!d->nav)
        return 0;
    if (dvdnav_current_title_info(d->nav, &title, &part) == DVDNAV_STATUS_OK) {
        d->title = title;
        d->part = part;
    }
    if (dvdnav_is_domain_vmgm(d->nav))
        d->domain = DVD_DOMAIN_VMGM;
    else if (dvdnav_is_domain_vtsm(d->nav))
        d->domain = DVD_DOMAIN_VTSMenu;
    else if (dvdnav_is_domain_vts(d->nav))
        d->domain = DVD_DOMAIN_VTSTitle;
    else if (dvdnav_is_domain_fp(d->nav))
        d->domain = DVD_DOMAIN_FirstPlay;
    if (d->title == 0)
        return 1;
    if (d->domain == DVD_DOMAIN_VMGM || d->domain == DVD_DOMAIN_VTSMenu)
        return 1;
    return 0;
}

static void dvdio_refresh_program(DVDIO *d)
{
    int32_t title = 0, pgcn = 0, pgn = 0;

    dvdio_detect_menu(d);
    if (d->nav &&
        dvdnav_current_title_program(d->nav, &title, &pgcn, &pgn)
            == DVDNAV_STATUS_OK) {
        d->pgcn = pgcn;
        d->pgn = pgn;
    }
}

static int dvdio_live_pci(DVDIO *d, pci_t *out)
{
    pci_t *live;

    if (!d || !d->nav || !out)
        return 0;
    live = dvdnav_get_current_nav_pci(d->nav);
    if (!live)
        return 0;
    *out = *live;
    return 1;
}

static int32_t dvdio_current_highlight(DVDIO *d)
{
    int32_t button = 0;

    if (!d->nav ||
        dvdnav_get_current_highlight(d->nav, &button) != DVDNAV_STATUS_OK)
        return 0;
    return button;
}

static void dvdio_activate_trace_event(DVDIO *d, int32_t event)
{
    int left, same_still;
    int32_t hl;

    if (!d->activate_trace)
        return;
    dvdio_refresh_program(d);
    hl = dvdio_current_highlight(d);
    dbg("DVD MENU: post-activate event %-16s  domain=%s title=%d part=%d "
        "cell=%d pg=%d pgcn=%d pgn=%d hl=%d still_flag=%u gen=%u\n",
        dvdnav_event_name(event),
        dvd_domain_name(d->domain),
        (int)d->title, (int)d->part,
        d->cellN, d->pgN, d->pgcn, d->pgn,
        (int)hl,
        d->nav ? dvdnav_get_next_still_flag(d->nav) : 0,
        d->player ? player_nav_gen(d->player) : 0);

    left = (event == DVDNAV_HOP_CHANNEL || event == DVDNAV_VTS_CHANGE ||
            d->domain != d->act_domain || d->title != d->act_title ||
            (event == DVDNAV_CELL_CHANGE && d->cellN != d->act_cellN));
    same_still = (event == DVDNAV_STILL_FRAME &&
                  d->domain == d->act_domain &&
                  d->title == d->act_title &&
                  (d->act_cellN <= 0 || d->cellN == d->act_cellN));
    if (left) {
        dbg("DVD MENU: post-activate: VM left the source menu state\n");
        d->activate_trace = 0;
    } else if (same_still) {
        dbg("DVD MENU: post-activate: STILL_FRAME in the same menu "
            "(activation did not leave this cell)\n");
        d->activate_trace = 0;
    }
}

static void menu_hl_request_redraw(Player *p)
{
    if (!p || !p->hl.inited)
        return;
    pthread_mutex_lock(&p->hl.mu);
    p->hl.redraw = 1;
    pthread_mutex_unlock(&p->hl.mu);
    if (p->buffered_yuv && p->yuvring.inited) {
        pthread_mutex_lock(&p->yuvring.mu);
        pthread_cond_signal(&p->yuvring.not_empty);
        pthread_mutex_unlock(&p->yuvring.mu);
    }
}

static int menu_spu_ensure_bufs(Player *p)
{
    if (!p)
        return -1;
    if (!p->spu.idx) {
        p->spu.idx = malloc(SPU_IDX_MAX);
        if (!p->spu.idx)
            return -1;
    }
    if (!p->spu.acc) {
        p->spu.acc = malloc(SPU_MAX_PKT);
        if (!p->spu.acc)
            return -1;
    }
    return 0;
}

static void menu_spu_invalidate(Player *p, const char *why)
{
    if (!p || !p->spu.inited)
        return;
    pthread_mutex_lock(&p->spu.mu);
    if (p->spu.valid)
        spu_dbg("SPU: invalidated on nav transition%s%s\n",
                why && *why ? " (" : "",
                why && *why ? why : "");
    p->spu.valid = 0;
    p->spu.w = p->spu.h = 0;
    p->spu.acc_size = 0;
    p->spu.tile_valid = 0;
    p->spu.tile_n = 0;
    p->spu.tile_dirty = 1;
    pthread_mutex_unlock(&p->spu.mu);
    movie_sub_reset(p, why);
}

static void menu_tile_mark_dirty(Player *p)
{
    if (p && p->spu.inited)
        p->spu.tile_dirty = 1;
}

static int menu_spu_nibble(const uint8_t *buf, int size, int *bitpos)
{
    int byte = *bitpos >> 3;
    int shift;

    if (byte >= size)
        return 0;
    shift = 4 - (*bitpos & 4);
    *bitpos += 4;
    return (buf[byte] >> shift) & 0xf;
}

static int menu_spu_run_2bit(const uint8_t *buf, int size, int *bitpos,
                             int *color)
{
    unsigned v = 0, t;

    for (t = 1; v < t && t <= 0x40; t <<= 2)
        v = (v << 4) | (unsigned)menu_spu_nibble(buf, size, bitpos);
    *color = (int)(v & 3);
    if (v < 4)
        return 0x7fffffff;
    return (int)(v >> 2);
}

static int menu_spu_decode_rle(uint8_t *bitmap, int linesize, int w, int h,
                               const uint8_t *buf, int start, int buf_size)
{
    int bitpos = start * 8;
    int x = 0, y = 0, len, color, bit_len, i;
    uint8_t *d;

    if (start < 0 || start >= buf_size || w <= 0 || h <= 0)
        return -1;
    bit_len = (buf_size - start) * 8;
    d = bitmap;
    for (;;) {
        if (bitpos - start * 8 > bit_len)
            return -1;
        len = menu_spu_run_2bit(buf, buf_size, &bitpos, &color);
        if (len != 0x7fffffff && len > w - x)
            return -1;
        if (len > w - x)
            len = w - x;
        for (i = 0; i < len; i++)
            d[x + i] = (uint8_t)color;
        x += len;
        if (x >= w) {
            y++;
            if (y >= h)
                break;
            d += linesize;
            x = 0;
            bitpos = (bitpos + 7) & ~7;
        }
    }
    return 0;
}

static uint32_t menu_yuv_to_bgr0(uint32_t yuv)
{
    int y = (int)((yuv >> 16) & 0xff);
    int cr = (int)((yuv >> 8) & 0xff) - 128;
    int cb = (int)(yuv & 0xff) - 128;
    int r = y + ((351 * cr) >> 8);
    int g = y - ((179 * cr + 86 * cb) >> 8);
    int b = y + ((443 * cb) >> 8);

    if (r < 0)
        r = 0;
    else if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    else if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    else if (b > 255)
        b = 255;
    return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16);
}

static void menu_clut_store(Player *p, const uint32_t *clut)
{
    int i;

    if (!p || !clut)
        return;
    pthread_mutex_lock(&p->spu.mu);
    memcpy(p->spu.clut, clut, 16 * sizeof(uint32_t));
    for (i = 0; i < 16; i++)
        p->spu.clut_bgr0[i] = menu_yuv_to_bgr0(clut[i]);
    p->spu.clut_valid = 1;
    p->spu.tile_dirty = 1;
    pthread_mutex_unlock(&p->spu.mu);
    spu_dbg("SPU: CLUT changed\n");
    sub_dbg("SUBTITLE CLUT CHANGE\n");
}

static int menu_spu_decode_unit(Player *p, const uint8_t *buf, int buf_size,
                                unsigned gen)
{
    int cmd_pos, pos, cmd, x1, y1, x2, y2, next_cmd_pos;
    uint8_t colormap[4] = {0, 1, 2, 3};
    uint8_t alpha[4] = {0, 0, 0, 0};
    int offset1 = -1, offset2 = -1;
    int w, h, size;

    if (buf_size < 10 || AV_RB16(buf) == 0)
        return -1;
    size = AV_RB16(buf);
    if (size < 10 || size > buf_size)
        return 1;
    cmd_pos = AV_RB16(buf + 2);
    if (cmd_pos < 4 || cmd_pos > buf_size - 4)
        return -1;

    while (cmd_pos > 0 && cmd_pos < buf_size - 4) {
        next_cmd_pos = AV_RB16(buf + cmd_pos + 2);
        pos = cmd_pos + 4;
        offset1 = offset2 = -1;
        x1 = y1 = x2 = y2 = 0;
        while (pos < buf_size) {
            cmd = buf[pos++];
            switch (cmd) {
            case 0x00:
                break;
            case 0x01:
            case 0x02:
                break;
            case 0x03:
                if (buf_size - pos < 2)
                    return -1;
                colormap[3] = buf[pos] >> 4;
                colormap[2] = buf[pos] & 0x0f;
                colormap[1] = buf[pos + 1] >> 4;
                colormap[0] = buf[pos + 1] & 0x0f;
                pos += 2;
                break;
            case 0x04:
                if (buf_size - pos < 2)
                    return -1;
                alpha[3] = buf[pos] >> 4;
                alpha[2] = buf[pos] & 0x0f;
                alpha[1] = buf[pos + 1] >> 4;
                alpha[0] = buf[pos + 1] & 0x0f;
                pos += 2;
                break;
            case 0x05:
                if (buf_size - pos < 6)
                    return -1;
                x1 = (buf[pos] << 4) | (buf[pos + 1] >> 4);
                x2 = ((buf[pos + 1] & 0x0f) << 8) | buf[pos + 2];
                y1 = (buf[pos + 3] << 4) | (buf[pos + 4] >> 4);
                y2 = ((buf[pos + 4] & 0x0f) << 8) | buf[pos + 5];
                pos += 6;
                break;
            case 0x06:
                if (buf_size - pos < 4)
                    return -1;
                offset1 = AV_RB16(buf + pos);
                offset2 = AV_RB16(buf + pos + 2);
                pos += 4;
                break;
            case 0xff:
                goto cmds_done;
            default:
                goto cmds_done;
            }
        }
    cmds_done:
        if (offset1 >= 0 && offset2 >= 0 && offset1 < buf_size &&
            offset2 < buf_size) {
            w = x2 - x1 + 1;
            h = y2 - y1 + 1;
            if (w > 0 && h > 1 && w <= FB_W && h <= FB_H) {
                if (menu_spu_ensure_bufs(p) < 0)
                    return -1;
                pthread_mutex_lock(&p->spu.mu);
                if (menu_spu_decode_rle(p->spu.idx, w * 2, w, (h + 1) / 2,
                                        buf, offset1, buf_size) < 0 ||
                    menu_spu_decode_rle(p->spu.idx + w, w * 2, w, h / 2,
                                        buf, offset2, buf_size) < 0) {
                    pthread_mutex_unlock(&p->spu.mu);
                    return -1;
                }
                p->spu.x = x1;
                p->spu.y = y1;
                p->spu.w = w;
                p->spu.h = h;
                memcpy(p->spu.color, colormap, 4);
                memcpy(p->spu.alpha, alpha, 4);
                p->spu.gen = gen;
                p->spu.valid = 1;
                p->spu.tile_dirty = 1;
                pthread_mutex_unlock(&p->spu.mu);
                spu_dbg("SPU: decoded x=%d y=%d w=%d h=%d colors=%d/%d/%d/%d\n",
                        x1, y1, w, h,
                        colormap[0], colormap[1], colormap[2], colormap[3]);
                return 0;
            }
        }
        if (next_cmd_pos <= cmd_pos)
            break;
        cmd_pos = next_cmd_pos;
    }
    return -1;
}

static void menu_spu_feed_packet(Player *p, const uint8_t *data, int size,
                                 unsigned gen)
{
    int need, got;

    if (!p || !p->spu.inited || !data || size <= 0)
        return;
    if (menu_spu_ensure_bufs(p) < 0)
        return;
    if (p->spu.acc_size > 0) {
        if (p->spu.acc_size + size > SPU_MAX_PKT) {
            p->spu.acc_size = 0;
            return;
        }
        memcpy(p->spu.acc + p->spu.acc_size, data, (size_t)size);
        p->spu.acc_size += size;
        data = p->spu.acc;
        size = p->spu.acc_size;
    }
    if (size < 2)
        return;
    need = AV_RB16(data);
    if (need < 10 || need > SPU_MAX_PKT)
        return;
    if (size < need) {
        if (p->spu.acc_size == 0) {
            memcpy(p->spu.acc, data, (size_t)size);
            p->spu.acc_size = size;
        }
        return;
    }
    got = menu_spu_decode_unit(p, data, size, gen);
    p->spu.acc_size = 0;
    if (got == 0)
        menu_hl_request_redraw(p);
}

static int menu_spu_packet_wanted(Player *p, AVFormatContext *fmt, AVPacket *pkt)
{
    AVStream *st;
    int pes, want;

    if (!p || !fmt || !pkt || pkt->stream_index < 0 ||
        (unsigned)pkt->stream_index >= fmt->nb_streams)
        return 0;
    st = fmt->streams[pkt->stream_index];
    if (!st || !st->codecpar)
        return 0;
    pes = st->id & 0xff;
    if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        if (pes < 0x20 || pes > 0x3f)
            pes = 0x20 + (st->id & 0x1f);
    } else if (pes < 0x20 || pes > 0x3f) {
        return 0;
    }
    want = p->spu.pes_id;
    if (want >= 0)
        return pes == want;
    return 1;
}

static void menu_spu_log_streams(Player *p, AVFormatContext *fmt)
{
    unsigned i;
    AVStream *st;
    AVDictionaryEntry *lang;
    const char *cname;
    unsigned bit;

    if (!p || !fmt || !g_debug_spu)
        return;
    for (i = 0; i < fmt->nb_streams; i++) {
        st = fmt->streams[i];
        if (!st || !st->codecpar)
            continue;
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE &&
            ((st->id & 0xff) < 0x20 || (st->id & 0xff) > 0x3f))
            continue;
        bit = (i < 32) ? (1u << i) : 0;
        if (bit && (p->spu.streams_logged & bit))
            continue;
        if (bit)
            p->spu.streams_logged |= bit;
        lang = av_dict_get(st->metadata, "language", NULL, 0);
        cname = avcodec_get_name(st->codecpar->codec_id);
        spu_dbg("SPU STREAM:\n"
                "  AVStream idx=%u\n"
                "  codec=%s\n"
                "  st->id=0x%x\n"
                "  language %s\n",
                i,
                cname ? cname : "?",
                st->id,
                lang && lang->value ? lang->value : "(none)");
        spu_dbg("SPU: AVStream idx=%u id=0x%x\n", i, st->id);
    }
}

static void menu_spu_note_decoder(Player *p)
{
    const AVCodec *dec;

    if (!p || p->spu.decoder_logged)
        return;
    p->spu.decoder_logged = 1;
    dec = avcodec_find_decoder(AV_CODEC_ID_DVD_SUBTITLE);
    if (dec)
        spu_dbg("SPU: decoder=dvdsub\n");
    else
        spu_dbg("SPU: decoder=rle (dvdsub not in this FFmpeg build)\n");
}

static void menu_spu_set_stream(Player *p, int logical, int wide, int letterbox)
{
    int phys, old_pes;

    if (!p)
        return;
    old_pes = p->spu.pes_id;
    p->spu.logical = logical;
    p->spu.physical_wide = wide;
    p->spu.physical_letterbox = letterbox;
    phys = wide;
    if (phys < 0)
        phys = letterbox;
    if (phys >= 0)
        p->spu.pes_id = 0x20 | (phys & 0x1f);
    if (old_pes != p->spu.pes_id) {
        pthread_mutex_lock(&p->spu.mu);
        p->spu.valid = 0;
        p->spu.acc_size = 0;
        p->spu.tile_valid = 0;
        p->spu.tile_n = 0;
        p->spu.tile_dirty = 1;
        pthread_mutex_unlock(&p->spu.mu);
    }
    spu_dbg("SPU: AVStream id=0x%x logical=%d wide=%d letterbox=%d\n",
            p->spu.pes_id, logical, wide, letterbox);
}

static void menu_blend_pixel(uint8_t *dst, uint32_t src_bgr0, int a8)
{
    uint32_t d;
    unsigned ia, b, g, r, db, dg, dr, ua;

    if (a8 <= 0)
        return;
    if (a8 >= 255) {
        *(uint32_t *)dst = src_bgr0;
        return;
    }
    ua = (unsigned)a8;
    ia = 255u - ua;
    d = *(uint32_t *)dst;
    db = d & 0xffu;
    dg = (d >> 8) & 0xffu;
    dr = (d >> 16) & 0xffu;
    b = ((src_bgr0 & 0xffu) * ua + db * ia + 127u) / 255u;
    g = (((src_bgr0 >> 8) & 0xffu) * ua + dg * ia + 127u) / 255u;
    r = (((src_bgr0 >> 16) & 0xffu) * ua + dr * ia + 127u) / 255u;
    *(uint32_t *)dst = b | (g << 8) | (r << 16);
}

static int menu_tile_grow(Player *p, int need)
{
    void *nbuf;
    int cap = p->spu.tile_cap;

    if (need <= cap)
        return 0;
    cap = cap ? cap * 2 : 256;
    while (cap < need)
        cap *= 2;
    nbuf = realloc(p->spu.tile_px, (size_t)cap * sizeof(*p->spu.tile_px));
    if (!nbuf)
        return -1;
    p->spu.tile_px = nbuf;
    p->spu.tile_cap = cap;
    return 0;
}

static void menu_overlay_rebuild_tile(Player *p, int vis, int sx, int sy,
                                      int ex, int ey, uint32_t pal,
                                      unsigned gen)
{
    int x0, y0, w, h, max_h, ix0, iy0, ix1, iy1, px, py, n;
    const uint8_t *idx;

    p->spu.tile_valid = 0;
    p->spu.tile_n = 0;
    if (!vis || !p->spu.valid || !p->spu.idx || !p->spu.clut_valid)
        return;
    if (p->spu.gen != gen)
        return;
    if (ex <= sx || ey <= sy || p->spu.w <= 0 || p->spu.h <= 0)
        return;

    x0 = p->spu.x;
    y0 = p->spu.y;
    w = p->spu.w;
    h = p->spu.h;
    idx = p->spu.idx;
    max_h = player_active_h(p);

    ix0 = x0 > sx ? x0 : sx;
    iy0 = y0 > sy ? y0 : sy;
    ix1 = (x0 + w - 1) < ex ? (x0 + w - 1) : ex;
    iy1 = (y0 + h - 1) < ey ? (y0 + h - 1) : ey;
    if (ix0 < 0)
        ix0 = 0;
    if (iy0 < 0)
        iy0 = 0;
    if (ix1 >= FB_W)
        ix1 = FB_W - 1;
    if (iy1 >= max_h)
        iy1 = max_h - 1;
    if (ix1 < ix0 || iy1 < iy0)
        return;

    n = 0;
    for (py = iy0; py <= iy1; py++) {
        for (px = ix0; px <= ix1; px++) {
            int code = idx[(py - y0) * w + (px - x0)] & 3;
            int a4 = (int)((pal >> (4 * code)) & 0xf);
            int ci;
            uint32_t bgr;

            if (a4 <= 0)
                continue;
            if (menu_tile_grow(p, n + 1) < 0)
                return;
            ci = (int)((pal >> (16 + 4 * code)) & 0xf);
            bgr = p->spu.clut_bgr0[ci];
            p->spu.tile_px[n].x = (uint16_t)px;
            p->spu.tile_px[n].y = (uint16_t)py;
            p->spu.tile_px[n].a8 = (uint8_t)(a4 * 17);
            p->spu.tile_px[n].pad = 0;
            p->spu.tile_px[n].bgr = bgr;
            n++;
        }
    }
    p->spu.tile_x = ix0;
    p->spu.tile_y = iy0;
    p->spu.tile_w = ix1 - ix0 + 1;
    p->spu.tile_h = iy1 - iy0 + 1;
    p->spu.tile_n = n;
    p->spu.tile_valid = 1;
    p->spu.tile_dirty = 0;
}

static void menu_overlay_composite(Player *p, uint8_t *dst, int stride,
                                   int frame_menu)
{
    int vis, in_menu, sx, sy, ex, ey, activated, i, n;
    unsigned hl_gen;
    uint32_t pal, pal_act;
    unsigned bbox;

    if (!p || !dst || g_debug_yellow_highlight)
        return;
    if (!p->hl.inited || !p->spu.inited)
        return;

    pthread_mutex_lock(&p->hl.mu);
    vis = p->hl.visible;
    in_menu = p->hl.in_menu;
    hl_gen = p->hl.gen;
    sx = p->hl.sx;
    sy = p->hl.sy;
    ex = p->hl.ex;
    ey = p->hl.ey;
    pal = p->hl.palette;
    pal_act = p->hl.palette_act;
    activated = p->hl.activated;
    pthread_mutex_unlock(&p->hl.mu);

    if (!in_menu || !frame_menu || hl_gen != player_nav_gen(p))
        return;

    if (activated && pal_act)
        pal = pal_act;

    pthread_mutex_lock(&p->spu.mu);
    if (p->spu.tile_dirty || !p->spu.tile_valid)
        menu_overlay_rebuild_tile(p, vis, sx, sy, ex, ey, pal, hl_gen);
    n = p->spu.tile_valid ? p->spu.tile_n : 0;
    bbox = (p->spu.tile_valid && p->spu.tile_w > 0 && p->spu.tile_h > 0)
           ? (unsigned)p->spu.tile_w * (unsigned)p->spu.tile_h : 0;
    for (i = 0; i < n; i++) {
        int px = (int)p->spu.tile_px[i].x;
        int py = (int)p->spu.tile_px[i].y;
        menu_blend_pixel(dst + (size_t)py * (size_t)stride + (size_t)px * 4,
                         p->spu.tile_px[i].bgr, p->spu.tile_px[i].a8);
    }
    pthread_mutex_unlock(&p->spu.mu);

    p->spu_perf.px_sum += (uint64_t)n;
    p->spu_perf.bbox_sum += (uint64_t)bbox;
    if (bbox > p->spu_perf.bbox_max)
        p->spu_perf.bbox_max = bbox;
}

/* DCSQ date is 90 kHz ticks of (date * 1024). Packet PTS and video PTS
 * are converted to microseconds first; convert this delay to us too:
 *   (date * 1024) / 90000 s  →  date * 1024 * 1e6 / 90000 us. */
static int64_t movie_sub_dcsq_delay_us(unsigned date)
{
    return av_rescale((int64_t)date * 1024, 1000000, 90000);
}

static int movie_sub_ensure_bufs(Player *p)
{
    if (!p)
        return -1;
    if (!p->msub.idx) {
        p->msub.idx = malloc(SPU_IDX_MAX);
        if (!p->msub.idx)
            return -1;
    }
    if (!p->msub.acc) {
        p->msub.acc = malloc(SPU_MAX_PKT);
        if (!p->msub.acc)
            return -1;
    }
    return 0;
}

static void movie_sub_reset(Player *p, const char *why)
{
    int was_valid;

    (void)why;
    if (!p || !p->msub.inited)
        return;
    pthread_mutex_lock(&p->msub.mu);
    was_valid = p->msub.valid || p->msub.visible_now;
    p->msub.valid = 0;
    p->msub.forced = 0;
    p->msub.shown = 0;
    p->msub.visible_now = 0;
    p->msub.w = p->msub.h = 0;
    p->msub.acc_size = 0;
    p->msub.packet_pts_us = AV_NOPTS_VALUE;
    p->msub.from_us = AV_NOPTS_VALUE;
    p->msub.until_us = AV_NOPTS_VALUE;
    p->msub.acc_pts_us = AV_NOPTS_VALUE;
    p->msub.evt_n = 0;
    p->msub.chg_n = 0;
    p->msub.saw_chg_colcon = 0;
    pthread_mutex_unlock(&p->msub.mu);
    if (was_valid)
        sub_dbg("SUBTITLE CLEAR\n");
}

static void movie_sub_set_stream(Player *p, int logical, int wide, int letterbox,
                                 int pan_scan, int active)
{
    int phys, old_pes;

    if (!p || !p->msub.inited)
        return;
    old_pes = p->msub.pes_id;
    p->msub.logical = logical;
    p->msub.physical_wide = wide;
    p->msub.physical_letterbox = letterbox;
    p->msub.physical_pan_scan = pan_scan;
    if (wide >= 0)
        phys = wide;
    else if (letterbox >= 0)
        phys = letterbox;
    else if (pan_scan >= 0)
        phys = pan_scan;
    else if (active >= 0)
        phys = active & 0x1f;
    else
        phys = -1;
    p->msub.chosen_physical = phys;
    if (phys >= 0)
        p->msub.pes_id = 0x20 | (phys & 0x1f);
    else
        p->msub.pes_id = -1;
    if (old_pes != p->msub.pes_id) {
        pthread_mutex_lock(&p->msub.mu);
        p->msub.valid = 0;
        p->msub.visible_now = 0;
        p->msub.shown = 0;
        p->msub.acc_size = 0;
        p->msub.acc_pts_us = AV_NOPTS_VALUE;
        pthread_mutex_unlock(&p->msub.mu);
    }
    sub_dbg("SUBTITLE STREAM CHANGE logical=%d wide=%d letterbox=%d "
            "pan_scan=%d active=%d chosen_physical=%d pes_id=%d\n",
            logical, wide, letterbox, pan_scan, active, phys, p->msub.pes_id);
    sub_dbg("SUBTITLE MAP:\n"
            "  logical=%d\n"
            "  physical_wide=%d\n"
            "  physical_letterbox=%d\n"
            "  physical_pan_scan=%d\n"
            "  chosen_physical=%d\n",
            p->msub.logical,
            p->msub.physical_wide,
            p->msub.physical_letterbox,
            p->msub.physical_pan_scan,
            p->msub.chosen_physical);
}

static void movie_sub_prefer_first(DVDIO *d)
{
    int i;
    int8_t active;
    uint16_t lang;
    char code[3];

    if (!d || !d->nav || !d->player || d->player->msub.prefer_done)
        return;
    if (d->player->in_menu)
        return;
    if (d->player->msub.pes_id >= 0) {
        d->player->msub.prefer_done = 1;
        return;
    }
    active = dvdnav_get_active_spu_stream(d->nav);
    if (active >= 0) {
        d->player->msub.prefer_done = 1;
        return;
    }
    d->player->msub.prefer_done = 1;
    for (i = 0; i < 32; i++) {
        lang = dvdnav_spu_stream_to_lang(d->nav, (uint8_t)i);
        if (lang == 0xffff)
            continue;
        code[0] = (char)((lang >> 8) & 0xff);
        code[1] = (char)(lang & 0xff);
        code[2] = 0;
        if (!code[0] || !code[1])
            continue;
        if (dvdnav_spu_language_select(d->nav, code) == DVDNAV_STATUS_OK)
            sub_dbg("SUBTITLE STREAM CHANGE preferred first logical=%d "
                    "lang=%c%c\n",
                    i, code[0], code[1]);
        return;
    }
}

static int movie_sub_packet_wanted(Player *p, AVFormatContext *fmt, AVPacket *pkt)
{
    AVStream *st;
    int pes, want;

    if (!p || !p->msub.inited || !fmt || !pkt || pkt->stream_index < 0 ||
        (unsigned)pkt->stream_index >= fmt->nb_streams)
        return 0;
    st = fmt->streams[pkt->stream_index];
    if (!st || !st->codecpar)
        return 0;
    pes = st->id & 0xff;
    if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        if (pes < 0x20 || pes > 0x3f)
            pes = 0x20 + (st->id & 0x1f);
    } else if (pes < 0x20 || pes > 0x3f) {
        return 0;
    }
    want = p->msub.pes_id;
    if (want < 0)
        return 0;
    return pes == want;
}

static void movie_sub_unpack_color(const uint8_t *b, uint8_t color[4])
{
    color[3] = b[0] >> 4;
    color[2] = b[0] & 0x0f;
    color[1] = b[1] >> 4;
    color[0] = b[1] & 0x0f;
}

static int64_t movie_sub_event_time(int64_t packet_pts_us, unsigned date)
{
    int64_t delay = movie_sub_dcsq_delay_us(date);

    if (packet_pts_us == AV_NOPTS_VALUE)
        return delay;
    return packet_pts_us + delay;
}

static int movie_sub_push_evt(MsubEvt *evts, int *evt_n, int64_t time_us,
                              uint8_t kind, const uint8_t *color,
                              const uint8_t *alpha, int chg_i)
{
    int i;

    if (*evt_n >= MSUB_EVT_MAX)
        return -1;
    i = (*evt_n)++;
    evts[i].time_us = time_us;
    evts[i].kind = kind;
    evts[i].chg_i = (uint8_t)chg_i;
    if (color)
        memcpy(evts[i].color, color, 4);
    else
        memset(evts[i].color, 0, 4);
    if (alpha)
        memcpy(evts[i].alpha, alpha, 4);
    else
        memset(evts[i].alpha, 0, 4);
    return 0;
}

/*
 * Parse CHG_COLCON parameter area starting at pos (the 2-byte size word).
 * Returns bytes consumed from pos (the size word value), or -1 if the size
 * word itself is unusable (caller must stop this DCSQ).
 */
static int movie_sub_parse_chg(Player *p, const uint8_t *buf, int buf_size,
                               int pos, int64_t packet_pts_us, unsigned date,
                               unsigned spu_id, MsubChg *chgs, int *chg_n,
                               MsubEvt *evts, int *evt_n, int *saw_chg)
{
    int size, end, cur, slot, bands, pxn, malformed = 0;
    int64_t event_time;
    MsubChg *g;

    p->msub.chg_colcon_events++;
    if (buf_size - pos < 2)
        return -1;
    size = AV_RB16(buf + pos);
    if (size < 2 || pos + size > buf_size) {
        p->msub.malformed_chg_colcon++;
        sub_dbg("SUBTITLE CHG_COLCON malformed spu_id=%u size=%d\n",
                spu_id, size);
        return -1;
    }
    event_time = movie_sub_event_time(packet_pts_us, date);
    end = pos + size;
    cur = pos + 2;
    slot = *chg_n;
    if (slot >= MSUB_CHG_MAX) {
        p->msub.malformed_chg_colcon++;
        sub_dbg("SUBTITLE CHG_COLCON malformed spu_id=%u reason=chg_cap\n",
                spu_id);
        return size;
    }
    g = &chgs[slot];
    memset(g, 0, sizeof(*g));
    bands = 0;
    pxn = 0;
    while (cur + 4 <= end) {
        uint32_t v = AV_RB32(buf + cur);
        int start_line, n_px, end_line, i;

        cur += 4;
        if (v == 0x0fffffffu)
            break;
        start_line = (int)((v >> 16) & 0xfff);
        n_px = (int)((v >> 12) & 0xf);
        end_line = (int)(v & 0xfff);
        if (n_px < 1 || n_px > 15 || start_line > end_line ||
            cur + n_px * 6 > end ||
            bands >= MSUB_BAND_MAX || pxn + n_px > MSUB_PX_MAX) {
            malformed = 1;
            break;
        }
        g->bands[bands].y0 = (uint16_t)start_line;
        g->bands[bands].y1 = (uint16_t)end_line;
        g->bands[bands].n_px = (uint8_t)n_px;
        g->bands[bands].px0 = (uint8_t)pxn;
        for (i = 0; i < n_px; i++) {
            g->px[pxn].start_col = AV_RB16(buf + cur);
            movie_sub_unpack_color(buf + cur + 2, g->px[pxn].color);
            movie_sub_unpack_color(buf + cur + 4, g->px[pxn].alpha);
            pxn++;
            cur += 6;
        }
        bands++;
    }
    if (malformed || bands <= 0) {
        p->msub.malformed_chg_colcon++;
        sub_dbg("SUBTITLE CHG_COLCON malformed spu_id=%u "
                "parameter_bytes=%d bands=%d\n",
                spu_id, size, bands);
        return size;
    }
    g->n_bands = (uint8_t)bands;
    g->n_px = (uint8_t)pxn;
    if (movie_sub_push_evt(evts, evt_n, event_time, MSUB_EVT_CHG, NULL, NULL,
                           slot) < 0) {
        p->msub.malformed_chg_colcon++;
        return size;
    }
    *chg_n = slot + 1;
    *saw_chg = 1;
    sub_dbg("SUBTITLE CHG_COLCON\n"
            "  spu_id=%u\n"
            "  packet_pts_us=%" PRId64 "\n"
            "  dcsq_date=%u\n"
            "  event_time_us=%" PRId64 "\n"
            "  parameter_bytes=%d\n"
            "  bands=%d\n",
            spu_id, packet_pts_us, date, event_time, size, bands);
    {
        int b, i;

        for (b = 0; b < bands; b++) {
            int px0 = g->bands[b].px0;
            int np = g->bands[b].n_px;

            sub_dbg("    lines=%u..%u\n"
                    "    px_entries=%d\n",
                    g->bands[b].y0, g->bands[b].y1, np);
            for (i = 0; i < np; i++) {
                const uint8_t *c = g->px[px0 + i].color;
                const uint8_t *a = g->px[px0 + i].alpha;

                sub_dbg("      start_col=%u\n"
                        "      color_map=%u/%u/%u/%u\n"
                        "      alpha=%u/%u/%u/%u\n",
                        g->px[px0 + i].start_col,
                        c[0], c[1], c[2], c[3],
                        a[0], a[1], a[2], a[3]);
            }
        }
    }
    return size;
}

static int movie_sub_decode_unit(Player *p, const uint8_t *buf, int buf_size,
                                 int64_t packet_pts_us)
{
    int cmd_pos, pos, cmd, x1, y1, x2, y2, next_cmd_pos;
    uint8_t colormap[4] = {0, 1, 2, 3};
    uint8_t alpha[4] = {0, 0, 0, 0};
    int offset1 = -1, offset2 = -1;
    int have_area = 0, have_offsets = 0;
    int start_date = -1, stop_date = -1, forced = 0;
    int w, h, size;
    unsigned spu_id;
    int saw_chg = 0, evt_n = 0, chg_n = 0;
    MsubEvt evts[MSUB_EVT_MAX];
    MsubChg chgs[MSUB_CHG_MAX];

    if (buf_size < 10 || AV_RB16(buf) == 0)
        return -1;
    size = AV_RB16(buf);
    if (size < 10 || size > buf_size)
        return -1;
    cmd_pos = AV_RB16(buf + 2);
    if (cmd_pos < 4 || cmd_pos > buf_size - 4)
        return -1;

    spu_id = ++p->msub.spu_seq;
    memset(evts, 0, sizeof(evts));
    memset(chgs, 0, sizeof(chgs));
    x1 = y1 = x2 = y2 = 0;
    while (cmd_pos > 0 && cmd_pos < buf_size - 4) {
        unsigned date = AV_RB16(buf + cmd_pos);
        int64_t evt_time = movie_sub_event_time(packet_pts_us, date);

        next_cmd_pos = AV_RB16(buf + cmd_pos + 2);
        pos = cmd_pos + 4;
        while (pos < buf_size) {
            cmd = buf[pos++];
            switch (cmd) {
            case 0x00:
                forced = 1;
                start_date = (int)date;
                break;
            case 0x01:
                start_date = (int)date;
                break;
            case 0x02:
                stop_date = (int)date;
                break;
            case 0x03:
                if (buf_size - pos < 2)
                    return -1;
                movie_sub_unpack_color(buf + pos, colormap);
                pos += 2;
                movie_sub_push_evt(evts, &evt_n, evt_time, MSUB_EVT_COLOR,
                                   colormap, NULL, 0);
                break;
            case 0x04:
                if (buf_size - pos < 2)
                    return -1;
                movie_sub_unpack_color(buf + pos, alpha);
                pos += 2;
                movie_sub_push_evt(evts, &evt_n, evt_time, MSUB_EVT_CONTR,
                                   NULL, alpha, 0);
                break;
            case 0x05:
                if (buf_size - pos < 6)
                    return -1;
                x1 = (buf[pos] << 4) | (buf[pos + 1] >> 4);
                x2 = ((buf[pos + 1] & 0x0f) << 8) | buf[pos + 2];
                y1 = (buf[pos + 3] << 4) | (buf[pos + 4] >> 4);
                y2 = ((buf[pos + 4] & 0x0f) << 8) | buf[pos + 5];
                pos += 6;
                have_area = 1;
                break;
            case 0x06:
                if (buf_size - pos < 4)
                    return -1;
                offset1 = AV_RB16(buf + pos);
                offset2 = AV_RB16(buf + pos + 2);
                pos += 4;
                have_offsets = 1;
                break;
            case 0x07: {
                int n = movie_sub_parse_chg(p, buf, buf_size, pos,
                                            packet_pts_us, date, spu_id,
                                            chgs, &chg_n, evts, &evt_n,
                                            &saw_chg);

                if (n < 0)
                    goto cmds_done;
                pos += n;
                break;
            }
            case 0xff:
                goto cmds_done;
            default:
                goto cmds_done;
            }
        }
    cmds_done:
        if (next_cmd_pos <= cmd_pos)
            break;
        cmd_pos = next_cmd_pos;
    }

    if (have_offsets && have_area && offset1 >= 0 && offset2 >= 0 &&
        offset1 < buf_size && offset2 < buf_size) {
        w = x2 - x1 + 1;
        h = y2 - y1 + 1;
        if (w > 0 && h > 1 && w <= FB_W && h <= FB_H) {
            if (movie_sub_ensure_bufs(p) < 0)
                return -1;
            pthread_mutex_lock(&p->msub.mu);
            if (menu_spu_decode_rle(p->msub.idx, w * 2, w, (h + 1) / 2,
                                    buf, offset1, buf_size) < 0 ||
                menu_spu_decode_rle(p->msub.idx + w, w * 2, w, h / 2,
                                    buf, offset2, buf_size) < 0) {
                pthread_mutex_unlock(&p->msub.mu);
                return -1;
            }
            p->msub.x = x1;
            p->msub.y = y1;
            p->msub.w = w;
            p->msub.h = h;
            p->msub.top_off = offset1;
            p->msub.bot_off = offset2;
            memcpy(p->msub.color, colormap, 4);
            memcpy(p->msub.alpha, alpha, 4);
            memcpy(p->msub.evt, evts, sizeof(evts));
            memcpy(p->msub.chg, chgs, sizeof(chgs));
            p->msub.evt_n = evt_n;
            p->msub.chg_n = chg_n;
            p->msub.saw_chg_colcon = saw_chg;
            p->msub.valid = 1;
            p->msub.forced = forced;
            p->msub.shown = 0;
            p->msub.last_spu_id = spu_id;
            p->msub.packet_pts_us = packet_pts_us;
            if (packet_pts_us != AV_NOPTS_VALUE) {
                p->msub.from_us = packet_pts_us;
                if (start_date >= 0)
                    p->msub.from_us = packet_pts_us +
                        movie_sub_dcsq_delay_us((unsigned)start_date);
                if (stop_date >= 0)
                    p->msub.until_us = packet_pts_us +
                        movie_sub_dcsq_delay_us((unsigned)stop_date);
                else
                    p->msub.until_us = AV_NOPTS_VALUE;
            } else {
                p->msub.from_us = AV_NOPTS_VALUE;
                p->msub.until_us = AV_NOPTS_VALUE;
            }
            pthread_mutex_unlock(&p->msub.mu);
            p->msub.decoded_spus++;
            p->msub.bbox_w_sum += (unsigned)w;
            p->msub.bbox_h_sum += (unsigned)h;
            p->msub.bbox_n++;
            if (w > (int)p->msub.bbox_w_max)
                p->msub.bbox_w_max = (unsigned)w;
            if (h > (int)p->msub.bbox_h_max)
                p->msub.bbox_h_max = (unsigned)h;
            sub_dbg("SUBTITLE RECT x=%d y=%d w=%d h=%d\n", x1, y1, w, h);
            if (saw_chg)
                p->msub.chg_colcon_spus++;
            sub_dbg("SUBTITLE DCSQ start=%" PRId64 " stop=%" PRId64
                    " forced=%d\n",
                    p->msub.from_us, p->msub.until_us, forced);
            sub_dbg("SUBTITLE SPU id=%u\n"
                    "  from_us=%" PRId64 "\n"
                    "  until_us=%" PRId64 "\n"
                    "  saw_chg_colcon=%s\n",
                    spu_id, p->msub.from_us, p->msub.until_us,
                    saw_chg ? "yes" : "no");
            return 0;
        }
    }

    if (stop_date >= 0 && p->msub.valid) {
        pthread_mutex_lock(&p->msub.mu);
        if (packet_pts_us != AV_NOPTS_VALUE)
            p->msub.until_us = packet_pts_us +
                movie_sub_dcsq_delay_us((unsigned)stop_date);
        pthread_mutex_unlock(&p->msub.mu);
        sub_dbg("SUBTITLE DCSQ start=%" PRId64 " stop=%" PRId64 " forced=%d\n",
                p->msub.from_us, p->msub.until_us, p->msub.forced);
        return 0;
    }

    return -1;
}

static void movie_sub_feed_packet(Player *p, const uint8_t *data, int size,
                                  int64_t pts_us)
{
    int need, got, leftover;

    if (!p || !p->msub.inited || !data || size <= 0)
        return;
    if (movie_sub_ensure_bufs(p) < 0)
        return;

    p->msub.spu_fragments++;
    if (p->msub.acc_size == 0) {
        p->msub.acc_pts_us = pts_us;
        sub_dbg("SUBTITLE SPU begin size=%d pts=%" PRId64 "\n",
                size, pts_us);
    }

    if (p->msub.acc_size > 0) {
        if (p->msub.acc_size + size > SPU_MAX_PKT) {
            p->msub.malformed_spus++;
            p->msub.acc_size = 0;
            p->msub.acc_pts_us = AV_NOPTS_VALUE;
            return;
        }
        memcpy(p->msub.acc + p->msub.acc_size, data, (size_t)size);
        p->msub.acc_size += size;
    } else {
        if (size > SPU_MAX_PKT) {
            p->msub.malformed_spus++;
            return;
        }
        memcpy(p->msub.acc, data, (size_t)size);
        p->msub.acc_size = size;
    }

    while (p->msub.acc_size >= 2) {
        need = AV_RB16(p->msub.acc);
        if (need < 10 || need > SPU_MAX_PKT) {
            p->msub.malformed_spus++;
            p->msub.acc_size = 0;
            p->msub.acc_pts_us = AV_NOPTS_VALUE;
            return;
        }
        if (p->msub.acc_size < need)
            return;
        sub_dbg("SUBTITLE SPU complete size=%d pts=%" PRId64 " leftover=%d\n",
                need, p->msub.acc_pts_us, p->msub.acc_size - need);
        p->msub.complete_spus++;
        got = movie_sub_decode_unit(p, p->msub.acc, need, p->msub.acc_pts_us);
        if (got < 0)
            p->msub.malformed_spus++;
        leftover = p->msub.acc_size - need;
        if (leftover > 0)
            memmove(p->msub.acc, p->msub.acc + need, (size_t)leftover);
        p->msub.acc_size = leftover;
        p->msub.acc_pts_us = AV_NOPTS_VALUE;
    }
}

static int movie_sub_visible_at(const Player *p, int64_t pvpts_us)
{
    if (!p->msub.valid || p->msub.w <= 0 || p->msub.h <= 0)
        return 0;
    if (pvpts_us == AV_NOPTS_VALUE)
        return 1;
    if (p->msub.from_us != AV_NOPTS_VALUE && pvpts_us < p->msub.from_us)
        return 0;
    if (p->msub.until_us != AV_NOPTS_VALUE && pvpts_us >= p->msub.until_us)
        return 0;
    return 1;
}

static int movie_sub_would_be_visible(Player *p, int64_t vpts_us)
{
    int vis;

    if (!p || !p->msub.inited || p->in_menu)
        return 0;
    pthread_mutex_lock(&p->msub.mu);
    vis = movie_sub_visible_at(p, vpts_us);
    pthread_mutex_unlock(&p->msub.mu);
    return vis;
}

static void movie_sub_state_at(const Player *p, int64_t now,
                               uint8_t color[4], uint8_t alpha[4],
                               const MsubChg **chg)
{
    static const uint8_t def_c[4] = {0, 1, 2, 3};
    static const uint8_t def_a[4] = {0, 0, 0, 0};
    int i;

    memcpy(color, def_c, 4);
    memcpy(alpha, def_a, 4);
    *chg = NULL;
    if (p->msub.evt_n <= 0) {
        memcpy(color, p->msub.color, 4);
        memcpy(alpha, p->msub.alpha, 4);
        return;
    }
    for (i = 0; i < p->msub.evt_n; i++) {
        if (now != AV_NOPTS_VALUE && p->msub.evt[i].time_us > now)
            continue;
        switch (p->msub.evt[i].kind) {
        case MSUB_EVT_COLOR:
            memcpy(color, p->msub.evt[i].color, 4);
            break;
        case MSUB_EVT_CONTR:
            memcpy(alpha, p->msub.evt[i].alpha, 4);
            break;
        case MSUB_EVT_CHG:
            if (p->msub.evt[i].chg_i < p->msub.chg_n)
                *chg = &p->msub.chg[p->msub.evt[i].chg_i];
            break;
        default:
            break;
        }
    }
}

static void movie_sub_maps_xy(const uint8_t base_c[4], const uint8_t base_a[4],
                              const MsubChg *chg, int px, int py,
                              uint8_t color[4], uint8_t alpha[4])
{
    int b, i, found;

    memcpy(color, base_c, 4);
    memcpy(alpha, base_a, 4);
    if (!chg)
        return;
    for (b = 0; b < chg->n_bands; b++) {
        if (py < (int)chg->bands[b].y0 || py > (int)chg->bands[b].y1)
            continue;
        found = -1;
        for (i = 0; i < chg->bands[b].n_px; i++) {
            const MsubPx *e = &chg->px[chg->bands[b].px0 + i];

            if ((int)e->start_col <= px)
                found = i;
            else
                break;
        }
        if (found >= 0) {
            const MsubPx *e = &chg->px[chg->bands[b].px0 + found];

            memcpy(color, e->color, 4);
            memcpy(alpha, e->alpha, 4);
        }
        return;
    }
}

static int movie_sub_overlay(Player *p, uint8_t *dst, int stride,
                             int64_t pvpts_us, int64_t *blend_us_out)
{
    int x0, y0, w, h, x, y, vis, max_h;
    uint8_t color[4], alpha[4];
    uint32_t clut[16];
    int clut_ok;
    uint8_t *idx;
    const MsubChg *chg;
    int64_t t0;

    if (blend_us_out)
        *blend_us_out = 0;
    if (!p || !dst || !p->msub.inited)
        return 0;
    if (p->in_menu)
        return 0;

    pthread_mutex_lock(&p->spu.mu);
    clut_ok = p->spu.clut_valid;
    if (clut_ok)
        memcpy(clut, p->spu.clut_bgr0, sizeof(clut));
    pthread_mutex_unlock(&p->spu.mu);
    if (!clut_ok)
        return 0;

    pthread_mutex_lock(&p->msub.mu);
    vis = movie_sub_visible_at(p, pvpts_us);
    if (vis && !p->msub.visible_now) {
        p->msub.visible_now = 1;
        if (!p->msub.shown) {
            p->msub.shown = 1;
            p->msub.displayed_spus++;
        }
        pthread_mutex_unlock(&p->msub.mu);
        sub_dbg("SUBTITLE ACTIVE\n");
        pthread_mutex_lock(&p->msub.mu);
        vis = movie_sub_visible_at(p, pvpts_us);
    } else if (!vis && p->msub.visible_now) {
        p->msub.visible_now = 0;
        pthread_mutex_unlock(&p->msub.mu);
        sub_dbg("SUBTITLE CLEAR\n");
        return 0;
    }
    if (!vis) {
        pthread_mutex_unlock(&p->msub.mu);
        return 0;
    }

    x0 = p->msub.x;
    y0 = p->msub.y;
    w = p->msub.w;
    h = p->msub.h;
    idx = p->msub.idx;
    movie_sub_state_at(p, pvpts_us, color, alpha, &chg);
    max_h = player_active_h(p);
    if (w <= 0 || h <= 0 || !idx) {
        pthread_mutex_unlock(&p->msub.mu);
        return 0;
    }
    t0 = av_gettime_relative();
    for (y = 0; y < h; y++) {
        int py = y0 + y;

        if (py < 0 || py >= max_h)
            continue;
        for (x = 0; x < w; x++) {
            int px = x0 + x;
            int code, a4, ci;
            uint8_t cc[4], aa[4];
            uint8_t *pix;

            if (px < 0 || px >= FB_W)
                continue;
            code = idx[y * w + x] & 3;
            if (chg) {
                movie_sub_maps_xy(color, alpha, chg, px, py, cc, aa);
                a4 = aa[code] & 0xf;
                ci = cc[code] & 0xf;
            } else {
                a4 = alpha[code] & 0xf;
                ci = color[code] & 0xf;
            }
            if (a4 <= 0)
                continue;
            pix = dst + (size_t)py * (size_t)stride + (size_t)px * 4;
            menu_blend_pixel(pix, clut[ci], a4 * 17);
        }
    }
    if (blend_us_out)
        *blend_us_out = av_gettime_relative() - t0;
    pthread_mutex_unlock(&p->msub.mu);
    return 1;
}

static void menu_hl_clear(Player *p)
{
    if (!p || !p->hl.inited)
        return;
    pthread_mutex_lock(&p->hl.mu);
    p->hl.visible = 0;
    p->hl.button = 0;
    p->hl.redraw = 0;
    p->hl.in_menu = 0;
    p->hl.still = 0;
    p->hl.activated = 0;
    p->hl.palette = 0;
    p->hl.palette_act = 0;
    pthread_mutex_unlock(&p->hl.mu);
    p->in_menu = 0;
    p->still_active = 0;
    p->menu_still_drop = 1;
    menu_spu_invalidate(p, "menu exit");
}

static void dvdio_leave_menu(DVDIO *d)
{
    int was_visible = 0;
    Player *p = d ? d->player : NULL;

    if (p && p->hl.inited) {
        pthread_mutex_lock(&p->hl.mu);
        was_visible = p->hl.visible || p->hl.in_menu;
        pthread_mutex_unlock(&p->hl.mu);
    } else if (p) {
        was_visible = p->in_menu;
    }
    menu_hl_clear(p);
    if (d)
        d->menu_exiting = 1;
    if (was_visible && (!d || !d->hl_leave_logged)) {
        fprintf(stderr, "DVD MENU: leaving menu - highlight cleared\n");
        if (d)
            d->hl_leave_logged = 1;
    }
}

static void dvdio_apply_highlight(DVDIO *d, int log_event)
{
    Player *p = d->player;
    int32_t button = 0;
    dvdnav_highlight_area_t area;
    dvdnav_highlight_area_t area_act;
    int btn_ns = 0;
    int in_menu;

    if (!d->nav)
        return;
    in_menu = dvdio_detect_menu(d);
    if (d->menu_exiting)
        in_menu = 0;
    if (d->pci_valid)
        btn_ns = d->pci.hli.hl_gi.btn_ns & 0x3f;
    if (dvdnav_get_current_highlight(d->nav, &button) != DVDNAV_STATUS_OK)
        button = 0;
    memset(&area, 0, sizeof(area));
    memset(&area_act, 0, sizeof(area_act));
    if (d->pci_valid && button > 0 &&
        dvdnav_get_highlight_area(&d->pci, button, HL_MODE_SELECT, &area)
            == DVDNAV_STATUS_OK) {
        /* SELECT area/palette filled from copied PCI, not a live pointer. */
    } else if (button <= 0) {
        memset(&area, 0, sizeof(area));
    }
    if (d->pci_valid && button > 0)
        dvdnav_get_highlight_area(&d->pci, button, HL_MODE_ACTION, &area_act);

    if (p && p->hl.inited) {
        int changed;

        pthread_mutex_lock(&p->hl.mu);
        changed = (p->hl.button != button) ||
                  (p->hl.sx != (int)area.sx) || (p->hl.sy != (int)area.sy) ||
                  (p->hl.ex != (int)area.ex) || (p->hl.ey != (int)area.ey) ||
                  (p->hl.palette != area.palette) ||
                  (p->hl.in_menu != in_menu);
        p->hl.visible = in_menu && button > 0 &&
                        (area.ex > area.sx) && (area.ey > area.sy);
        p->hl.button = (int)button;
        p->hl.btn_ns = btn_ns;
        p->hl.sx = (int)area.sx;
        p->hl.sy = (int)area.sy;
        p->hl.ex = (int)area.ex;
        p->hl.ey = (int)area.ey;
        p->hl.palette = area.palette;
        p->hl.palette_act = area_act.palette;
        p->hl.pci_lbn = d->pci_valid ? d->pci_lbn : 0;
        p->hl.hli_ss = d->pci_valid ? (int)(d->pci.hli.hl_gi.hli_ss & 0x03) : 0;
        p->hl.activated = 0;
        p->hl.gen = player_nav_gen(p);
        p->hl.in_menu = in_menu;
        p->hl.still = d->still_len != 0;
        pthread_mutex_unlock(&p->hl.mu);
        p->in_menu = in_menu;
        p->still_active = d->still_len != 0;
        if (in_menu && changed) {
            menu_tile_mark_dirty(p);
            menu_hl_request_redraw(p);
            spu_dbg("HLI: buttons=%d selected=%d\n"
                    "HLI: select area=(%u,%u)-(%u,%u) palette=%08x\n"
                    "HLI: redraw selected=%d\n",
                    btn_ns, (int)button,
                    (unsigned)area.sx, (unsigned)area.sy,
                    (unsigned)area.ex, (unsigned)area.ey,
                    (unsigned)area.palette,
                    (int)button);
        }
    }

    if (log_event) {
        dbg("DVD MENU HIGHLIGHT:\n"
            "  button=%d\n"
            "  area=(%u,%u)-(%u,%u)\n",
            (int)button,
            (unsigned)area.sx, (unsigned)area.sy,
            (unsigned)area.ex, (unsigned)area.ey);
        spu_dbg("HLI: buttons=%d selected=%d\n"
                "HLI: select area=(%u,%u)-(%u,%u) palette=%08x\n",
                btn_ns, (int)button,
                (unsigned)area.sx, (unsigned)area.sy,
                (unsigned)area.ex, (unsigned)area.ey,
                (unsigned)area.palette);
    }
}

static void player_nav_discontinuity(Player *p, const char *why)
{
    int n_a = 0, n_v = 0, n_y = 0;
    unsigned gen;

    if (!p)
        return;
    pause_cancel(p);
    gen = __atomic_add_fetch(&p->nav_gen, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&p->codec_gen, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&p->frames_this_nav_gen, 0, __ATOMIC_SEQ_CST);
    p->still_drain_req = 0;
    p->still_drain_done = 0;
    p->soft_decode_trace = 0;
    n_a = pktq_flush(&p->aq);
    n_v = pktq_flush(&p->vq);
    if (p->buffered_yuv)
        n_y = yuvring_flush(&p->yuvring);
    p->flush_n++;
    p->nav_flush_pkts += (unsigned long)(n_a + n_v);
    p->nav_flush_yuv += (unsigned long)n_y;
    p->video_reset_req = 1;
    p->audio_reset_req = 1;
    p->demux_reopen_req = 1;
    p->soft_nav_log = 0;
    p->soft_log_pkt = 0;
    p->soft_log_decode = 0;
    p->soft_log_yuv = 0;
    clock_reset(&p->clock);
    p->buf_mraudio_started = 0;
    p->pcm_hold_len = 0;
    p->first_audio_pts_us = AV_NOPTS_VALUE;
    p->last_audio_pts_us = AV_NOPTS_VALUE;
    p->last_video_pts_us = AV_NOPTS_VALUE;
    p->first_genuine_pts = AV_NOPTS_VALUE;
    p->timeline_pts = AV_NOPTS_VALUE;
    p->assigned_pts = AV_NOPTS_VALUE;
    p->initial_skip_left = p->initial_skip_req;
    if (player_buffered(p)) {
        pthread_mutex_lock(&p->prefill_mu);
        p->prefill_released = 0;
        p->prefill_got = 0;
        p->prefill_t0_us = av_gettime_relative();
        p->prefill_reason = NULL;
        pthread_cond_broadcast(&p->prefill_cv);
        pthread_mutex_unlock(&p->prefill_mu);
        pthread_mutex_lock(&p->aq.mu);
        pthread_cond_broadcast(&p->aq.not_empty);
        pthread_mutex_unlock(&p->aq.mu);
        pthread_mutex_lock(&p->vq.mu);
        pthread_cond_broadcast(&p->vq.not_empty);
        pthread_mutex_unlock(&p->vq.mu);
    }
    fprintf(stderr,
            "NAV RESET: HARD\n"
            "  reason=%s\n",
            why ? why : "domain/VTS/title change");
    dbg("DVD MENU: navigation jump (%s)\n"
        "  nav_gen=%u  codec_gen=%u  flush qA=%d qV=%d yuv=%d  "
        "total_flushes=%u\n",
        why ? why : "hop", gen, player_codec_gen(p), n_a, n_v, n_y, p->flush_n);
    menu_spu_invalidate(p, why);
}

static void player_nav_soft_reset(Player *p, const char *reason,
                                  const char *old_pos, const char *new_pos,
                                  int pending_avio)
{
    int n_v = 0, n_y = 0;
    unsigned gen;

    if (!p)
        return;
    pause_cancel(p);
    gen = __atomic_add_fetch(&p->nav_gen, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&p->frames_this_nav_gen, 0, __ATOMIC_SEQ_CST);
    p->still_drain_req = 0;
    p->still_drain_done = 0;
    p->soft_decode_trace = 1;
    n_v = pktq_flush(&p->vq);
    if (p->buffered_yuv)
        n_y = yuvring_flush(&p->yuvring);
    p->flush_n++;
    p->nav_flush_pkts += (unsigned long)n_v;
    p->nav_flush_yuv += (unsigned long)n_y;
    /* Presentation gen invalidates old YUV/highlight. Codec parameters
     * are unchanged: do not flush/reinit MPEG-2 parser or decoder. */
    p->menu_still_drop = 1;
    p->soft_nav_log = 1;
    p->soft_nav_gen = gen;
    p->soft_log_pkt = 1;
    p->soft_log_decode = 1;
    p->soft_log_yuv = 1;
    /* Same-domain menu hop: keep MPEG-PS/AVIO, audio, clock, and skip
     * arming. The new VOBU's pending bytes must reach FFmpeg. */
    if (player_buffered(p)) {
        pthread_mutex_lock(&p->prefill_mu);
        p->prefill_released = 0;
        p->prefill_got = 0;
        p->prefill_t0_us = av_gettime_relative();
        p->prefill_reason = NULL;
        pthread_cond_broadcast(&p->prefill_cv);
        pthread_mutex_unlock(&p->prefill_mu);
        pthread_mutex_lock(&p->vq.mu);
        pthread_cond_broadcast(&p->vq.not_empty);
        pthread_mutex_unlock(&p->vq.mu);
    }
    fprintf(stderr,
            "NAV RESET: SOFT\n"
            "  reason=%s\n"
            "  old domain/pgcn/cell=%s\n"
            "  new domain/pgcn/cell=%s\n"
            "  pending_avio_bytes=%d\n",
            reason ? reason : "same-domain menu hop",
            old_pos ? old_pos : "?",
            new_pos ? new_pos : "?",
            pending_avio);
    dbg("DVD MENU: navigation reset: SOFT same-domain menu hop\n"
        "  nav_gen=%u  codec_gen=%u (unchanged)  flush qV=%d yuv=%d  "
        "(parser/decoder preserved)\n",
        gen, player_codec_gen(p), n_v, n_y);
    menu_spu_invalidate(p, reason);
}

static void dvdio_snapshot_pci(DVDIO *d)
{
    pci_t live;

    if (!dvdio_live_pci(d, &live))
        return;
    d->pci = live;
    d->pci_valid = 1;
    d->pci_lbn = live.pci_gi.nv_pck_lbn;
    d->pci_gen = d->player ? player_nav_gen(d->player) : 0;
}

static int dvdio_nav_status(dvdnav_t *nav, dvdnav_status_t st, const char *what)
{
    if (st == DVDNAV_STATUS_OK)
        return 1;
    fprintf(stderr, "DVD MENU: %s failed: %s\n",
            what, dvdnav_err_to_string(nav));
    return 0;
}

static void dvdio_log_button(DVDIO *d)
{
    int32_t button = 0;

    if (dvdnav_get_current_highlight(d->nav, &button) == DVDNAV_STATUS_OK)
        fprintf(stderr, "DVD MENU: selected button %d\n", (int)button);
    dvdio_apply_highlight(d, 0);
}

static void dvdio_end_local_still(DVDIO *d)
{
    /* Exit our still-wait loop only. Do not call dvdnav_still_skip(): that
     * sets skip_still + sync_wait_skip, which survive HOP and can skip the
     * destination's first cell still / FIFO WAIT. button_activate already
     * clears position_current.still on a jump. */
    d->still_len = 0;
    d->still_armed = 0;
}

static void dvdio_menu_to_title(DVDIO *d, const char *why, int reopen)
{
    if (!d || !d->player)
        return;
    d->soft_hop_active = 0;
    dvdio_leave_menu(d);
    if (!d->hop_pending)
        player_nav_discontinuity(d->player, why);
    if (reopen)
        d->hop_pending = 1;
}

static void dvdio_menu_resume(DVDIO *d)
{
    fprintf(stderr, "DVD MENU: resume requested\n");
    if (dvdio_nav_status(d->nav,
                         dvdnav_menu_call(d->nav, DVD_MENU_Escape),
                         "Escape/resume")) {
        dvdio_end_local_still(d);
        /* Domain change / HOP performs leave_menu + discontinuity. */
    }
}

static void dvdio_activate_button(DVDIO *d)
{
    int32_t button = 0, hl_now = 0;
    pci_t pci;
    pci_t stale;
    int have_stale = 0;
    dvdnav_status_t st;
    unsigned gen;
    uint32_t still_before, still_after;
    int32_t title_b, part_b, pgcn_b, pgn_b;
    int32_t title_a, part_a, pgcn_a, pgn_a;
    DVDDomain_t dom_b, dom_a;
    int cell_b, pg_b, btn_ns, hli_ss;
    char cmdbuf[192];
    btni_t *btni;

    if (!dvdio_live_pci(d, &pci)) {
        fprintf(stderr, "DVD MENU: activate ERR: no current NAV PCI\n");
        return;
    }
    if (d->pci_valid) {
        stale = d->pci;
        have_stale = 1;
    }
    d->pci = pci;
    d->pci_valid = 1;
    d->pci_lbn = pci.pci_gi.nv_pck_lbn;
    d->pci_gen = d->player ? player_nav_gen(d->player) : 0;

    dvdio_refresh_program(d);
    gen = d->player ? player_nav_gen(d->player) : 0;
    hl_now = dvdio_current_highlight(d);
    button = hl_now;
    if (button <= 0 && d->player && d->player->hl.inited) {
        pthread_mutex_lock(&d->player->hl.mu);
        button = d->player->hl.button;
        pthread_mutex_unlock(&d->player->hl.mu);
    }
    btn_ns = (int)(pci.hli.hl_gi.btn_ns & 0x3f);
    hli_ss = (int)(pci.hli.hl_gi.hli_ss & 0x03);
    if (d->player && d->player->hl.inited && button > 0) {
        dvdnav_highlight_area_t area_act;

        memset(&area_act, 0, sizeof(area_act));
        if (dvdnav_get_highlight_area(&pci, button, HL_MODE_ACTION, &area_act)
                == DVDNAV_STATUS_OK) {
            pthread_mutex_lock(&d->player->hl.mu);
            d->player->hl.activated = 1;
            d->player->hl.palette_act = area_act.palette;
            pthread_mutex_unlock(&d->player->hl.mu);
            menu_tile_mark_dirty(d->player);
            menu_hl_request_redraw(d->player);
        }
    }
    if (button <= 0) {
        fprintf(stderr, "DVD MENU: activate ERR: no selected button\n");
        return;
    }
    if (!hli_ss || button > btn_ns) {
        fprintf(stderr, "DVD MENU: activate ERR: live PCI has hli_ss=%d "
                "btn_ns=%d button=%d nv_pck_lbn=%u\n",
                hli_ss, btn_ns, (int)button,
                (unsigned)pci.pci_gi.nv_pck_lbn);
        return;
    }

    still_before = dvdnav_get_next_still_flag(d->nav);
    title_b = d->title;
    part_b = d->part;
    pgcn_b = d->pgcn;
    pgn_b = d->pgn;
    dom_b = d->domain;
    cell_b = d->cellN;
    pg_b = d->pgN;
    btni = &pci.hli.btnit[button - 1];
    dvd_vm_cmd_describe(&btni->cmd, cmdbuf, sizeof(cmdbuf));

    fprintf(stderr, "DVD MENU: activate button %d\n", (int)button);
    dbg("DVD MENU: ACTIVATE BEFORE\n"
        "  domain=%s  title=%d part=%d  cell=%d pg=%d pgcn=%d pgn=%d\n"
        "  highlight=%d  activate_button=%d  btn_ns=%d  hli_ss=%d\n"
        "  live PCI nv_pck_lbn=%u  stored PCI nv_pck_lbn=%s%u  pci_gen=%u  "
        "nav_gen=%u\n"
        "  still_len=%d  still_flag=%u  auto_action=%u\n"
        "  VM cmd: %s\n",
        dvd_domain_name(dom_b),
        (int)title_b, (int)part_b, cell_b, pg_b, pgcn_b, pgn_b,
        (int)hl_now, (int)button, btn_ns, hli_ss,
        (unsigned)pci.pci_gi.nv_pck_lbn,
        have_stale ? "" : "(none) ",
        have_stale ? (unsigned)stale.pci_gi.nv_pck_lbn : 0,
        d->pci_gen, gen,
        d->still_len, still_before, btni->auto_action_mode,
        cmdbuf);
    if (have_stale && stale.pci_gi.nv_pck_lbn != pci.pci_gi.nv_pck_lbn) {
        dbg("DVD MENU: discarded stale PCI snapshot nv_pck_lbn=%u "
            "(live=%u) — activating current menu NAV only\n",
            (unsigned)stale.pci_gi.nv_pck_lbn,
            (unsigned)pci.pci_gi.nv_pck_lbn);
    }
    if (d->pci_gen && d->pci_gen != gen) {
        dbg("DVD MENU: live PCI gen=%u != current nav_gen=%u\n",
            d->pci_gen, gen);
    }

    d->act_domain = dom_b;
    d->act_title = title_b;
    d->act_cellN = cell_b;
    d->activate_trace = 1;

    st = dvdnav_button_select_and_activate(d->nav, &pci, button);

    dvdio_refresh_program(d);
    still_after = dvdnav_get_next_still_flag(d->nav);
    title_a = d->title;
    part_a = d->part;
    pgcn_a = d->pgcn;
    pgn_a = d->pgn;
    dom_a = d->domain;
    hl_now = dvdio_current_highlight(d);

    if (st != DVDNAV_STATUS_OK) {
        fprintf(stderr, "DVD MENU: activate ERR: %s\n",
                dvdnav_err_to_string(d->nav));
        d->activate_trace = 0;
        return;
    }
    fprintf(stderr, "DVD MENU: activate OK\n");
    dbg("DVD MENU: ACTIVATE AFTER  status=OK (%s)\n"
        "  domain=%s  title=%d part=%d  cell=%d pg=%d pgcn=%d pgn=%d\n"
        "  highlight=%d  still_flag=%u  still_len=%d  hop_pending=%d\n"
        "  VM hop inferred: %s  (libdvdnav returns OK even if the command "
        "did not increment hop_channel)\n",
        dvdnav_err_to_string(d->nav),
        dvd_domain_name(dom_a),
        (int)title_a, (int)part_a, d->cellN, d->pgN, pgcn_a, pgn_a,
        (int)hl_now, still_after, d->still_len, d->hop_pending,
        (dom_a != dom_b || title_a != title_b || pgcn_a != pgcn_b ||
         pgn_a != pgn_b || d->cellN != cell_b)
            ? "yes (domain/title/PGC/cell changed immediately)"
            : "no (same domain/title/PGC/cell; hop_channel not observable "
              "via public API)");

    /* Do not dvdnav_still_skip and do not leave_menu here. Activate already
     * cleared the menu still and (on a jump) incremented hop_channel.
     * still_skip would leave skip_still set into the title; leave_menu would
     * mark in_menu=0 before HOP/VTS, so the present thread would clock-wait
     * on leftover menu pixels. Exit only the local still-wait so
     * get_next_block can emit HOP/VTS/CELL. */
    dvdio_end_local_still(d);
    dbg("DVD MENU: activate waiting for HOP/VTS/CELL or same-menu STILL "
        "(no still_skip)\n");
}

static void dvdio_chapter_step(DVDIO *d, int dir)
{
    int32_t title = 0, part = 0, parts = 0, target;

    if (!d->nav)
        return;
    if (dvdnav_current_title_info(d->nav, &title, &part) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "DVD CHAPTER: unavailable in menu\n");
        dbg("DVD CHAPTER: current_title_info failed: %s\n",
            dvdnav_err_to_string(d->nav));
        return;
    }
    d->title = title;
    d->part = part;
    if (title <= 0) {
        fprintf(stderr, "DVD CHAPTER: unavailable in menu\n");
        return;
    }
    if (dvdnav_get_number_of_parts(d->nav, title, &parts) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "DVD CHAPTER: get_number_of_parts failed: %s\n",
                dvdnav_err_to_string(d->nav));
        return;
    }
    dbg("DVD CHAPTER: title=%d part=%d parts=%d dir=%s\n",
        (int)title, (int)part, (int)parts, dir > 0 ? "NEXT" : "PREVIOUS");
    if (dir > 0) {
        if (part >= parts) {
            fprintf(stderr, "DVD CHAPTER: already at final chapter\n");
            return;
        }
        target = part + 1;
    } else {
        if (part <= 1) {
            fprintf(stderr, "DVD CHAPTER: already at first chapter\n");
            return;
        }
        target = part - 1;
    }
    if (dvdnav_part_play(d->nav, title, target) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "DVD CHAPTER: part_play(%d, %d) failed: %s\n",
                (int)title, (int)target, dvdnav_err_to_string(d->nav));
        return;
    }
    fprintf(stderr, "DVD CHAPTER: %d -> %d\n", (int)part, (int)target);
    d->skip_chapter_log = 1;
    dvdio_end_local_still(d);
    if (d->player)
        player_nav_discontinuity(d->player, "CHAPTER");
    d->hop_pending = 1;
    d->pci_valid = 0;
}

static const char *dvd_audio_fmt_name(uint16_t fmt)
{
    switch (fmt) {
    case DVD_AUDIO_FORMAT_AC3:
        return "AC3";
    case DVD_AUDIO_FORMAT_MPEG:
        return "MPEG";
    case DVD_AUDIO_FORMAT_MPEG2_EXT:
        return "MPEG";
    case DVD_AUDIO_FORMAT_LPCM:
        return "LPCM";
    case DVD_AUDIO_FORMAT_DTS:
        return "DTS";
    case DVD_AUDIO_FORMAT_SDDS:
        return "SDDS";
    default:
        return "unk";
    }
}

/* Formats enabled in the SS1 FFmpeg DVD prefix (ac3/eac3/mp1/mp2/mp3/dca/pcm_dvd). */
static int dvd_audio_fmt_supported(uint16_t fmt)
{
    switch (fmt) {
    case DVD_AUDIO_FORMAT_AC3:
    case DVD_AUDIO_FORMAT_MPEG:
    case DVD_AUDIO_FORMAT_MPEG2_EXT:
    case DVD_AUDIO_FORMAT_LPCM:
    case DVD_AUDIO_FORMAT_DTS:
        return 1;
    default:
        return 0;
    }
}

static void dvd_audio_lang_code(dvdnav_t *nav, int logical, char *out, size_t n)
{
    uint16_t lang;

    if (!out || n < 4)
        return;
    memcpy(out, "und", 4);
    if (!nav || logical < 0 || logical > 7)
        return;
    lang = dvdnav_audio_stream_to_lang(nav, (uint8_t)logical);
    if (lang == 0xffff)
        return;
    out[0] = (char)((lang >> 8) & 0xff);
    out[1] = (char)(lang & 0xff);
    out[2] = 0;
    if (out[0] < 32 || out[0] > 126)
        out[0] = '?';
    if (out[1] < 32 || out[1] > 126)
        out[1] = '?';
}

static int dvd_audio_pes_id(uint16_t fmt, int nav_phys)
{
    if (nav_phys < 0)
        return -1;
    if (nav_phys >= 0x80)
        return nav_phys & 0xff;
    if (nav_phys > 7)
        return nav_phys;
    switch (fmt) {
    case DVD_AUDIO_FORMAT_AC3:
        return 0x80 + nav_phys;
    case DVD_AUDIO_FORMAT_DTS:
        return 0x88 + nav_phys;
    case DVD_AUDIO_FORMAT_LPCM:
        return 0xa0 + nav_phys;
    case DVD_AUDIO_FORMAT_MPEG:
    case DVD_AUDIO_FORMAT_MPEG2_EXT:
        return 0xc0 + nav_phys;
    default:
        return nav_phys;
    }
}

static int dvd_audio_enum_valid(dvdnav_t *nav, int *out, int cap)
{
    int i, n = 0;

    if (!nav || !out || cap < 1)
        return 0;
    for (i = 0; i < 8 && n < cap; i++) {
        int8_t phys;
        uint16_t fmt;

        /* Header says physical→logical; 6.1.0 impl is logical→physical. */
        phys = dvdnav_get_audio_logical_stream(nav, (uint8_t)i);
        if (phys < 0)
            continue;
        fmt = dvdnav_audio_stream_format(nav, (uint8_t)i);
        if (fmt == 0xffff || !dvd_audio_fmt_supported(fmt))
            continue;
        out[n++] = i;
    }
    return n;
}

static int dvd_audio_logical_from_physical(dvdnav_t *nav, int physical)
{
    int i;

    if (!nav || physical < 0)
        return -1;
    for (i = 0; i < 8; i++) {
        int8_t phys = dvdnav_get_audio_logical_stream(nav, (uint8_t)i);

        if (phys >= 0 && (int)phys == physical)
            return i;
    }
    return -1;
}

static int dvdio_audio_in_list(const int *valid, int n, int logical)
{
    int i;

    if (logical < 0)
        return 0;
    for (i = 0; i < n; i++) {
        if (valid[i] == logical)
            return 1;
    }
    return 0;
}

static int dvdio_audio_resolve_logical(DVDIO *d, const int *valid, int n)
{
    Player *p;
    int phys, mapped;

    p = d->player;
    if (p && dvdio_audio_in_list(valid, n, p->current_audio_logical))
        return p->current_audio_logical;
    if (p && dvdio_audio_in_list(valid, n, p->audio_pending_logical))
        return p->audio_pending_logical;
    /* 6.1.0: dvdnav_get_active_audio_stream() is PHYSICAL, not ASTN. */
    phys = dvdnav_get_active_audio_stream(d->nav);
    mapped = dvd_audio_logical_from_physical(d->nav, phys);
    if (dvdio_audio_in_list(valid, n, mapped))
        return mapped;
    return -1;
}

static void dvdio_audio_log_context(const DVDIO *d)
{
    fprintf(stderr, "  domain=%s title=%d part=%d pgcn=%d pgn=%d vts=%d\n",
            dvd_domain_name(d->domain),
            (int)d->title, (int)d->part, d->pgcn, d->pgn, d->vtsN);
}

static void dvdio_audio_log_map(DVDIO *d)
{
    int i;
    int8_t active_phys;

    dvdio_refresh_program(d);
    fprintf(stderr, "AUDIO MAP:\n");
    dvdio_audio_log_context(d);
    active_phys = dvdnav_get_active_audio_stream(d->nav);
    for (i = 0; i < 8; i++) {
        int8_t phys;
        uint16_t fmt, ch;
        char langc[4];
        int pes;
        const char *skip = "";

        phys = dvdnav_get_audio_logical_stream(d->nav, (uint8_t)i);
        if (phys < 0)
            continue;
        fmt = dvdnav_audio_stream_format(d->nav, (uint8_t)i);
        ch = dvdnav_audio_stream_channels(d->nav, (uint8_t)i);
        dvd_audio_lang_code(d->nav, i, langc, sizeof(langc));
        pes = dvd_audio_pes_id(fmt, phys);
        if (fmt == 0xffff || !dvd_audio_fmt_supported(fmt))
            skip = " [skipped]";
        fprintf(stderr,
                "  logical %d -> physical 0x%02x (nav=%d) lang=%s %s %uch%s\n",
                i, pes < 0 ? 0 : pes, (int)phys, langc,
                dvd_audio_fmt_name(fmt),
                (ch == 0xffff) ? 0 : (unsigned)ch, skip);
    }
    fprintf(stderr,
            "  current_logical=%d pending_logical=%d active_physical=%d\n",
            d->player ? d->player->current_audio_logical : -1,
            d->player ? d->player->audio_pending_logical : -1,
            (int)active_phys);
}

static void dvdio_audio_reset_logical(DVDIO *d, const char *why)
{
    Player *p = d->player;

    if (!p)
        return;
    if (p->current_audio_logical < 0 && p->audio_pending_logical < 0)
        return;
    fprintf(stderr, "AUDIO MAP RESET: %s\n", why ? why : "nav change");
    dvdio_audio_log_context(d);
    p->current_audio_logical = -1;
    p->audio_pending_logical = -1;
}

/*
 * libdvdnav 6.1.0 has no dvdnav_audio_change(). Authored menus change
 * ASTN via SetSTN; execute the same VM command without a Link so there
 * is no hop. Encoding: system-set immediate subtype 1, audio specified.
 */
static void dvd_vm_setstn_audio(vm_cmd_t *cmd, int logical)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->bytes[0] = 0x51;
    cmd->bytes[3] = (uint8_t)(0x80 | (logical & 0x07));
}

static void dvdio_audio_next(DVDIO *d)
{
    int cur, next, i, n, idx, valid[8];
    char lang_from[4], lang_to[4];
    vm_cmd_t cmd;
    user_ops_t uops;

    if (!d || !d->nav) {
        fprintf(stderr, "AUDIO NEXT: ignored (no navigation)\n");
        return;
    }
    if (dvdio_detect_menu(d) || (d->player && d->player->in_menu)) {
        fprintf(stderr, "AUDIO NEXT: ignored (menu domain)\n");
        return;
    }
    if (!dvdnav_is_domain_vts(d->nav)) {
        fprintf(stderr, "AUDIO NEXT: ignored (not in title domain)\n");
        return;
    }
    uops = dvdnav_get_restrictions(d->nav);
    if (uops.audio_stream_change) {
        fprintf(stderr, "AUDIO NEXT: prohibited by disc\n");
        return;
    }

    n = dvd_audio_enum_valid(d->nav, valid, 8);
    dvdio_audio_log_map(d);
    if (n <= 0) {
        fprintf(stderr, "AUDIO NEXT: no valid audio streams\n");
        return;
    }
    if (n == 1) {
        fprintf(stderr, "AUDIO NEXT: only one audio stream\n");
        return;
    }

    cur = dvdio_audio_resolve_logical(d, valid, n);
    idx = -1;
    if (cur >= 0) {
        for (i = 0; i < n; i++) {
            if (valid[i] == cur) {
                idx = i;
                break;
            }
        }
    }
    next = (idx < 0) ? valid[0] : valid[(idx + 1) % n];
    if (next == cur) {
        fprintf(stderr, "AUDIO NEXT: only one audio stream\n");
        return;
    }

    dvd_audio_lang_code(d->nav, cur, lang_from, sizeof(lang_from));
    dvd_audio_lang_code(d->nav, next, lang_to, sizeof(lang_to));
    if (cur < 0)
        fprintf(stderr, "AUDIO NEXT REQUEST: logical ? unknown -> logical %d %s\n",
                next, lang_to);
    else
        fprintf(stderr, "AUDIO NEXT REQUEST: logical %d %s -> logical %d %s\n",
                cur, lang_from, next, lang_to);

    dvd_vm_setstn_audio(&cmd, next);
    if (dvdnav_button_activate_cmd(d->nav, 1, &cmd) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "AUDIO NEXT: VM SetSTN failed (%s)\n",
                dvdnav_err_to_string(d->nav));
        return;
    }
    if (d->player)
        d->player->audio_pending_logical = next;
}

static void dvdio_exec_nav_cmd(DVDIO *d, NavCmd cmd)
{
    int in_menu;
    pci_t *pci;

    if (!d->nav || cmd == NAVCMD_NONE)
        return;
    in_menu = dvdio_detect_menu(d);

    switch (cmd) {
    case NAVCMD_NONE:
        break;
    case NAVCMD_PLAY_PAUSE:
        if (in_menu || (d->player && (d->player->in_menu || d->player->still_active))) {
            fprintf(stderr, "DVD PAUSE: unavailable in menu\n");
            break;
        }
        if (d->player) {
            int was = pause_mode(d->player);

            if (was != PAUSE_OFF && g_debug_stats) {
                fprintf(stderr, "PAUSE wake: command=RESUME\n");
                pause_debug_threads(d->player, "resume");
            }
            pause_request_toggle(d->player);
            if (was != PAUSE_OFF && g_debug_stats)
                fprintf(stderr, "demux wake OK\n");
        }
        break;
    case NAVCMD_MENU:
        if (d->player) {
            int was = pause_mode(d->player);

            if (was != PAUSE_OFF && g_debug_stats) {
                fprintf(stderr, "PAUSE wake: command=MENU\n");
                pause_debug_threads(d->player, "menu");
            }
            pause_cancel(d->player);
            if (was != PAUSE_OFF && g_debug_stats)
                fprintf(stderr, "demux wake OK\n");
        }
        if (in_menu) {
            dvdio_menu_resume(d);
        } else {
            fprintf(stderr, "DVD MENU: Root requested\n");
            if (dvdnav_menu_call(d->nav, DVD_MENU_Root) != DVDNAV_STATUS_OK) {
                fprintf(stderr, "DVD MENU: Root unavailable, trying Title\n");
                dvdio_nav_status(d->nav,
                                 dvdnav_menu_call(d->nav, DVD_MENU_Title),
                                 "Title menu");
            }
            d->still_len = 0;
            d->still_armed = 0;
        }
        break;
    case NAVCMD_CANCEL:
        if (in_menu)
            dvdio_menu_resume(d);
        break;
    case NAVCMD_UP:
    case NAVCMD_DOWN:
    case NAVCMD_LEFT:
    case NAVCMD_RIGHT:
        if (!in_menu)
            break;
        if (!d->pci_valid) {
            dvdio_snapshot_pci(d);
            if (!d->pci_valid) {
                fprintf(stderr, "DVD MENU: no PCI snapshot for button nav\n");
                break;
            }
        }
        pci = &d->pci;
        if (cmd == NAVCMD_UP)
            dvdio_nav_status(d->nav,
                             dvdnav_upper_button_select(d->nav, pci), "UP");
        else if (cmd == NAVCMD_DOWN)
            dvdio_nav_status(d->nav,
                             dvdnav_lower_button_select(d->nav, pci), "DOWN");
        else if (cmd == NAVCMD_LEFT)
            dvdio_nav_status(d->nav,
                             dvdnav_left_button_select(d->nav, pci), "LEFT");
        else
            dvdio_nav_status(d->nav,
                             dvdnav_right_button_select(d->nav, pci), "RIGHT");
        dvdio_log_button(d);
        break;
    case NAVCMD_ACTIVATE:
        if (!in_menu) {
            dbg("DVD MENU: activate ignored (not in menu)\n");
            break;
        }
        dvdio_activate_button(d);
        break;
    case NAVCMD_PREVIOUS_CHAPTER:
        dvdio_chapter_step(d, -1);
        break;
    case NAVCMD_NEXT_CHAPTER:
        dvdio_chapter_step(d, 1);
        break;
    case NAVCMD_AUDIO_NEXT:
        dvdio_audio_next(d);
        break;
    default:
        break;
    }
}

static void dvdio_process_nav_cmds(DVDIO *d)
{
    NavCmd cmd;

    if (!d->player)
        return;
    while ((cmd = navq_pop(d->player)) != NAVCMD_NONE)
        dvdio_exec_nav_cmd(d, cmd);
}

static void dvdio_handle_wait(DVDIO *d)
{
    int64_t t0 = av_gettime_relative();

    dbg("DVDNAV WAIT\n");
    while (!g_interrupt && !d->stopped) {
        int vq = 0, yuv = 0;

        dvdio_process_nav_cmds(d);
        if (d->hop_pending)
            break;
        if (d->player) {
            vq = pktq_count(&d->player->vq);
            if (d->player->buffered_yuv)
                yuv = yuvring_count(&d->player->yuvring);
        } else {
            break;
        }
        if (vq == 0 && yuv == 0)
            break;
        if (av_gettime_relative() - t0 > NAV_WAIT_FIFO_US)
            break;
        av_usleep(5000);
    }
    dvdnav_wait_skip(d->nav);
    dbg("DVDNAV WAIT completed\n");
}

static void dvdio_handle_still(DVDIO *d, int length)
{
    int64_t t0 = av_gettime_relative();
    int64_t dur_us;
    int in_menu = d->player ? d->player->in_menu : 0;

    d->still_len = length;
    d->still_t0_us = t0;
    /*
     * MPEG bytes for this VOBU were already returned to FFmpeg (still_armed
     * with out>0 returns first). Drain parser/decoder so a lone still
     * I-frame is emitted before this thread blocks in the still wait.
     */
    if (d->player && in_menu && player_buffered(d->player) &&
        !player_menu_has_frame(d->player)) {
        player_request_still_drain(d->player);
        player_wait_still_drain(d->player, d);
    }
    if (!d->still_logged) {
        d->still_logged = 1;
        dbg("DVD MENU: still length=%s (%d)  gen=%u  title=%d part=%d "
            "domain=%s  buttons=%d  frames_this_gen=%u\n",
            length == 0xff ? "infinite" : "finite",
            length,
            d->player ? player_nav_gen(d->player) : 0,
            (int)d->title, (int)d->part,
            dvd_domain_name(d->domain),
            d->pci_valid ? (d->pci.hli.hl_gi.btn_ns & 0x3f) : 0,
            d->player
                ? __atomic_load_n(&d->player->frames_this_nav_gen,
                                  __ATOMIC_SEQ_CST)
                : 0);
        if (d->soft_hop_active)
            dbg("DVD MENU: STILL_FRAME after SOFT HOP\n"
                "  bytes_preserved=%d  first_BLOCK_OK=%s  "
                "first_mpeg_return=%s  still=%s\n",
                d->post_soft_bytes,
                d->post_soft_block_ok ? "yes" : "NO",
                d->post_soft_mpeg_ret ? "yes" : "NO",
                length == 0xff ? "infinite" : "finite");
    }
    if (d->player) {
        d->player->still_active = 1;
        if (player_buffered(d->player) && !prefill_is_released(d->player)) {
            int n = d->player->buffered_yuv
                    ? yuvring_count(&d->player->yuvring) : 0;
            /* Never release an empty YUV queue: the still VOBU must be
             * decoded first. Producer also releases on the first menu frame. */
            if (n >= 1)
                prefill_release(d->player, "DVDNAV_STILL_FRAME");
        }
        dvdio_apply_highlight(d, 0);
        pthread_mutex_lock(&d->player->clock.mu);
        pthread_cond_broadcast(&d->player->clock.ready_cv);
        pthread_mutex_unlock(&d->player->clock.mu);
    }

    if (length == 0xff)
        dur_us = 0;
    else
        dur_us = (int64_t)length * 1000000LL;

    while (!g_interrupt && !d->stopped && d->still_len) {
        dvdio_process_nav_cmds(d);
        if (d->hop_pending || d->still_len == 0)
            break;
        if (d->player && player_buffered(d->player) &&
            !prefill_is_released(d->player)) {
            int n = d->player->buffered_yuv
                    ? yuvring_count(&d->player->yuvring) : 0;
            if (n >= 1)
                prefill_release(d->player, "DVDNAV_STILL_FRAME");
        }
        if (length != 0xff &&
            av_gettime_relative() - t0 >= dur_us) {
            dvdnav_still_skip(d->nav);
            d->still_len = 0;
            break;
        }
        av_usleep(CTRL_POLL_US);
    }
    if (d->player)
        d->player->still_active = (d->still_len != 0);
}

static int player_menu_has_frame(Player *p)
{
    int n = 0;
    unsigned frames;

    if (!p)
        return 0;
    frames = __atomic_load_n(&p->frames_this_nav_gen, __ATOMIC_SEQ_CST);
    if (frames > 0)
        return 1;
    if (p->buffered_yuv)
        n = yuvring_count(&p->yuvring);
    return n >= 1;
}

static void player_request_still_drain(Player *p)
{
    unsigned gen;

    if (!p || !player_buffered(p))
        return;
    if (player_menu_has_frame(p))
        return;
    gen = player_nav_gen(p);
    p->still_drain_gen = gen;
    p->still_drain_done = 0;
    p->still_drain_req = 1;
    pktq_push_marker(&p->vq, VQ_MARK_STILL_BOUNDARY);
    dbg("DVD MENU: STILL-BOUNDARY marker queued  nav_gen=%u  "
        "codec_gen=%u  frames_this_gen=%u\n",
        gen, player_codec_gen(p),
        __atomic_load_n(&p->frames_this_nav_gen, __ATOMIC_SEQ_CST));
}

static void player_wait_still_drain(Player *p, DVDIO *d)
{
    int64_t t0;

    if (!p || !player_buffered(p))
        return;
    t0 = av_gettime_relative();
    while (!g_interrupt && !p->fail && !(d && d->stopped)) {
        int done;

        if (player_menu_has_frame(p))
            break;
        pthread_mutex_lock(&p->prefill_mu);
        done = p->still_drain_done;
        pthread_mutex_unlock(&p->prefill_mu);
        if (done)
            break;
        if (av_gettime_relative() - t0 >= STILL_DRAIN_WAIT_US) {
            dbg("DVD MENU: STILL-BOUNDARY drain wait timeout  "
                "nav_gen=%u  frames_this_gen=%u  yuv=%d\n",
                player_nav_gen(p),
                __atomic_load_n(&p->frames_this_nav_gen, __ATOMIC_SEQ_CST),
                p->buffered_yuv ? yuvring_count(&p->yuvring) : -1);
            break;
        }
        if (d)
            dvdio_process_nav_cmds(d);
        av_usleep(5000);
    }
}

static int dvdio_pump(DVDIO *d, int32_t event, int32_t len)
{
    int in_menu;

    if (event == DVDNAV_CELL_CHANGE) {
        dvdnav_cell_change_event_t *ev =
            (dvdnav_cell_change_event_t *)d->sector;
        if (ev) {
            d->cellN = ev->cellN;
            d->pgN = ev->pgN;
        }
    }
    if (d->activate_trace)
        dvdio_activate_trace_event(d, event);

    switch (event) {
    case DVDNAV_NAV_PACKET:
        d->nav_packets++;
        dvdio_snapshot_pci(d);
        if (len == DVD_SECTOR) {
            d->sector_len = len;
            d->sector_pos = 0;
            if (d->soft_hop_active && !d->post_soft_block_ok)
                dbg("DVD MENU: first NAV after SOFT HOP  len=%d  gen=%u\n",
                    len, d->player ? player_nav_gen(d->player) : 0);
        }
        in_menu = dvdio_detect_menu(d);
        if (d->player) {
            int was = d->player->in_menu;
            d->player->in_menu = in_menu;
            if (in_menu && !was) {
                movie_sub_reset(d->player, "enter menu");
                const char *kind = (d->title == 0)
                                   ? dvd_menu_id_name(d->part)
                                   : (d->domain == DVD_DOMAIN_VMGM
                                          ? "Root" : "Title");
                fprintf(stderr, "DVD MENU: entered %s\n", kind);
                d->hl_leave_logged = 0;
                d->menu_exiting = 0;
                dbg("DVD MENU: entered menu domain\n"
                    "  domain=%s  title=%d part=%d  gen=%u  buttons=%d\n",
                    dvd_domain_name(d->domain),
                    (int)d->title, (int)d->part,
                    player_nav_gen(d->player),
                    d->pci_valid ? (d->pci.hli.hl_gi.btn_ns & 0x3f) : 0);
            } else if (!in_menu && was) {
                dbg("DVD MENU: leaving menu domain\n"
                    "  domain=%s  title=%d part=%d  gen=%u\n",
                    dvd_domain_name(d->domain),
                    (int)d->title, (int)d->part,
                    player_nav_gen(d->player));
                dvdio_menu_to_title(d, "DVDNAV_NAV_PACKET", 0);
                movie_sub_prefer_first(d);
            } else if (in_menu) {
                d->hl_leave_logged = 0;
                d->menu_exiting = 0;
            }
            if (in_menu && !d->menu_exiting) {
                int8_t active = dvdnav_get_active_spu_stream(d->nav);
                if (d->player->spu.pes_id < 0 && active >= 0)
                    menu_spu_set_stream(d->player, -1, active & 0x1f, -1);
                dvdio_apply_highlight(d, 0);
            }
        }
        return 0;
    case DVDNAV_BLOCK_OK:
        if (len == DVD_SECTOR) {
            d->sector_len = len;
            d->sector_pos = 0;
            d->mpeg_sectors++;
            if (d->soft_hop_active && !d->post_soft_block_ok) {
                d->post_soft_block_ok = 1;
                dbg("DVD MENU: first BLOCK_OK after SOFT HOP  len=%d  "
                    "gen=%u\n",
                    len, d->player ? player_nav_gen(d->player) : 0);
            }
        }
        return 0;
    case DVDNAV_WAIT:
        if (!d->player) {
            dvdnav_wait_skip(d->nav);
            return 0;
        }
        d->wait_armed = 1;
        return 0;
    case DVDNAV_STILL_FRAME: {
        dvdnav_still_event_t *ev = (dvdnav_still_event_t *)d->sector;
        int slen = ev ? ev->length : 0xff;

        if (!d->player) {
            dvdnav_still_skip(d->nav);
            return 0;
        }
        d->still_len = slen;
        d->still_armed = 1;
        return 0;
    }
    case DVDNAV_VTS_CHANGE: {
        dvdnav_vts_change_event_t *ev = (dvdnav_vts_change_event_t *)d->sector;

        if (ev)
            d->domain = ev->new_domain;
        {
            int in_menu = dvdio_detect_menu(d);
            if (d->player) {
                int was = d->player->in_menu;
                d->player->in_menu = in_menu;
                if (was && !in_menu)
                    dvdio_leave_menu(d);
            }
            dbg("DVD MENU: VTS_CHANGE  %s -> %s  vts %d -> %d  "
                "title=%d part=%d  in_menu=%d\n",
                ev ? dvd_domain_name(ev->old_domain) : "?",
                dvd_domain_name(d->domain),
                ev ? ev->old_vtsN : -1,
                ev ? ev->new_vtsN : -1,
                (int)d->title, (int)d->part, in_menu);
        }
        if (ev)
            d->vtsN = ev->new_vtsN;
        dvdio_refresh_program(d);
        dvdio_audio_reset_logical(d, "VTS_CHANGE");
        if (d->player && !d->hop_pending)
            player_nav_discontinuity(d->player, "VTS_CHANGE (stream/VTS)");
        d->hop_pending = 1;
        d->soft_hop_active = 0;
        d->still_len = 0;
        d->still_logged = 0;
        d->still_armed = 0;
        d->wait_armed = 0;
        d->pci_valid = 0;
        return 0;
    }
    case DVDNAV_HOP_CHANNEL: {
        DVDDomain_t old_dom = d->domain;
        int old_pgcn = d->pgcn;
        int old_cell = d->cellN;
        int old_menu = d->player ? d->player->in_menu : 0;
        int pending = (d->sector_len > d->sector_pos)
                      ? (d->sector_len - d->sector_pos) : 0;
        int in_menu;
        int soft = 0;

        in_menu = dvdio_detect_menu(d);
        dvdio_refresh_program(d);
        dbg("DVD MENU: HOP_CHANNEL  domain=%s title=%d part=%d in_menu=%d\n",
            dvd_domain_name(d->domain), (int)d->title, (int)d->part, in_menu);
        if (d->player) {
            int was = d->player->in_menu;
            d->player->in_menu = in_menu;
            if (was && !in_menu)
                dvdio_leave_menu(d);
        }
        /* Same-domain menu page change is not a media discontinuity.
         * VTSMenu->VTSMenu / VMGM->VMGM keep MPEG-PS/AVIO so the new
         * still VOBU reaches FFmpeg. Menu<->title, VMGM<->VTSMenu, and
         * already-armed hard hops keep the full reopen. */
        if (!d->hop_pending && old_menu && in_menu &&
            old_dom == d->domain &&
            (d->domain == DVD_DOMAIN_VTSMenu ||
             d->domain == DVD_DOMAIN_VMGM))
            soft = 1;
        if (d->player && !d->hop_pending) {
            if (soft) {
                char oldbuf[80], newbuf[80];

                snprintf(oldbuf, sizeof(oldbuf), "%s/%d/%d",
                         dvd_domain_name(old_dom), old_pgcn, old_cell);
                snprintf(newbuf, sizeof(newbuf), "%s/%d/%d",
                         dvd_domain_name(d->domain), d->pgcn, d->cellN);
                player_nav_soft_reset(d->player,
                    d->domain == DVD_DOMAIN_VTSMenu
                        ? "same-domain VTSMenu HOP"
                        : "same-domain VMGM HOP",
                    oldbuf, newbuf, pending);
                d->soft_hop_active = 1;
                d->soft_drop_pre_hop = 1;
                d->post_soft_bytes = 0;
                d->post_soft_block_ok = 0;
                d->post_soft_mpeg_ret = 0;
                dbg("DVD MENU: bytes preserved through HOP  "
                    "sector_leftover=%d (not discarded; no AVIO drain)\n",
                    pending);
            } else {
                const char *why = "HOP_CHANNEL (media/VTS change)";

                if (old_menu && !in_menu)
                    why = "HOP VTSMenu/VMGM -> Title";
                else if (!old_menu && in_menu)
                    why = "HOP Title -> menu";
                else if (old_dom != d->domain)
                    why = "HOP domain change";
                player_nav_discontinuity(d->player, why);
                d->hop_pending = 1;
                d->soft_hop_active = 0;
            }
        } else if (d->hop_pending) {
            d->soft_hop_active = 0;
        }
        d->still_len = 0;
        d->still_logged = 0;
        d->still_armed = 0;
        d->wait_armed = 0;
        d->pci_valid = 0;
        return 0;
    }
    case DVDNAV_CELL_CHANGE: {
        dvdnav_cell_change_event_t *ev = (dvdnav_cell_change_event_t *)d->sector;
        int32_t prev_title = d->title;
        int32_t prev_part = d->part;
        int old_pgcn = d->pgcn;
        int now = dvdio_detect_menu(d);

        dvdio_refresh_program(d);
        if (d->player && d->player->in_menu && !now)
            dvdio_menu_to_title(d, "DVDNAV_CELL_CHANGE", 1);
        if (d->player)
            d->player->in_menu = now;
        if (d->player && now)
            movie_sub_reset(d->player, "CELL_CHANGE menu");
        else if (d->player && !now)
            movie_sub_prefer_first(d);
        if (d->title > 0 && d->part > 0 && prev_title > 0 &&
            prev_title == d->title && prev_part > 0 && prev_part != d->part) {
            if (d->skip_chapter_log)
                d->skip_chapter_log = 0;
            else
                fprintf(stderr, "DVD CHAPTER: %d -> %d\n",
                        (int)prev_part, (int)d->part);
        }
        d->last_title = d->title;
        d->last_part = d->part;
        if (old_pgcn != d->pgcn)
            dvdio_audio_reset_logical(d, "PGC_CHANGE");
        dbg("DVD MENU: CELL_CHANGE  cell=%d pg=%d  title=%d part=%d  "
            "domain=%s  in_menu=%d  still=%d\n",
            ev ? ev->cellN : -1, ev ? ev->pgN : -1,
            (int)d->title, (int)d->part,
            dvd_domain_name(d->domain),
            d->player ? d->player->in_menu : 0,
            d->still_len);
        return 0;
    }
    case DVDNAV_HIGHLIGHT:
        if (d->menu_exiting)
            return 0;
        dvdio_snapshot_pci(d);
        dvdio_apply_highlight(d, 1);
        return 0;
    case DVDNAV_SPU_STREAM_CHANGE: {
        dvdnav_spu_stream_change_event_t *ev =
            (dvdnav_spu_stream_change_event_t *)d->sector;
        int8_t active = d->nav ? dvdnav_get_active_spu_stream(d->nav) : -1;
        int wide = ev ? ev->physical_wide : -1;
        int letter = ev ? ev->physical_letterbox : -1;
        int pan = ev ? ev->physical_pan_scan : -1;
        int logical = ev ? ev->logical : -1;
        int menu_wide = wide;

        if (d->player && d->nav && logical < 0 && !d->player->in_menu) {
            uint16_t lang0 = dvdnav_spu_stream_to_lang(d->nav, 0);

            if (lang0 != 0xffff)
                logical = 0;
        }
        if (menu_wide < 0 && active >= 0)
            menu_wide = active & 0x1f;
        if (d->player)
            menu_spu_set_stream(d->player, logical, menu_wide, letter);
        if (d->player)
            movie_sub_set_stream(d->player, logical, wide, letter, pan,
                                 (int)active);
        if (d->player && !d->player->in_menu)
            movie_sub_prefer_first(d);
        dbg("DVDNAV_SPU_STREAM_CHANGE  logical=%d wide=%d letterbox=%d "
            "pan_scan=%d active=%d\n",
            logical, wide, letter, pan, (int)active);
        return 0;
    }
    case DVDNAV_SPU_CLUT_CHANGE:
        if (d->player)
            menu_clut_store(d->player, (const uint32_t *)d->sector);
        else
            dbg("DVDNAV_SPU_CLUT_CHANGE\n");
        return 0;
    case DVDNAV_AUDIO_STREAM_CHANGE: {
        dvdnav_audio_stream_change_event_t *ev =
            (dvdnav_audio_stream_change_event_t *)d->sector;
        uint16_t fmt = 0xffff;
        int pes = -1;
        int logical = ev ? ev->logical : -1;
        int physical = ev ? ev->physical : -1;

        dbg("DVD MENU: AUDIO_STREAM_CHANGE  physical=%d logical=%d\n",
            physical, logical);
        dvdio_refresh_program(d);
        if (logical >= 0 && logical <= 7)
            fmt = dvdnav_audio_stream_format(d->nav, (uint8_t)logical);
        pes = dvd_audio_pes_id(fmt, physical);
        fprintf(stderr,
                "AUDIO CHANGE EVENT: logical=%d physical=%d (PES 0x%02x)\n",
                logical, physical, pes < 0 ? 0 : pes);
        dvdio_audio_log_context(d);
        if (d->player && ev) {
            if (logical >= 0 && logical <= 7)
                d->player->current_audio_logical = logical;
            else
                d->player->current_audio_logical = -1;
            d->player->audio_pending_logical = -1;
            d->player->audio_follow_logical = logical;
            d->player->audio_follow_physical = physical;
            d->player->audio_follow_fmt =
                (fmt == 0xffff) ? -1 : (int)fmt;
        }
        return 0;
    }
    case DVDNAV_STOP:
        fprintf(stderr, "dvdnav: STOP (title/program end)\n");
        d->stopped = 1;
        return -1;
    case DVDNAV_NOP:
    default:
        return 0;
    }
}

static int read_sched_cpu(void)
{
#ifdef __linux__
    int c = sched_getcpu();
    return c;
#else
    return -1;
#endif
}

static void snapshot_available_cpus(Player *p)
{
    cpu_set_t set;
    long onln, conf;
    int i;

    onln = sysconf(_SC_NPROCESSORS_ONLN);
    conf = sysconf(_SC_NPROCESSORS_CONF);
    p->ncpu_onln = onln > 0 ? (int)onln : 0;
    p->ncpu_conf = conf > 0 ? (int)conf : 0;
    p->cpu_aff_mask = 0;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (i = 0; i < 32; i++) {
            if (CPU_ISSET(i, &set))
                p->cpu_aff_mask |= 1u << i;
        }
    }
    p->sched_video_cpu = -1;
    p->sched_audio_cpu = -1;
    p->sched_demux_cpu = -1;
    p->sched_present_cpu = -1;
    p->sched_input_cpu = -1;
}

static void note_unpinned_cpu(const char *name, int *got_cpu)
{
    *got_cpu = read_sched_cpu();
    dbg("sched: %s unpinned (sched_getcpu=%d)\n", name, *got_cpu);
}

static unsigned dvd_std_to_fpga_src(DvdVideoStd std)
{
    if (std == DVD_VIDEO_PAL)
        return FPGA_SRC_PAL;
    if (std == DVD_VIDEO_NTSC)
        return FPGA_SRC_NTSC;
    return FPGA_SRC_UNKNOWN;
}

static void dvd_fpga_poll_settings(Player *p)
{
    uint64_t w;
    unsigned raw;
    int trim;

    if (!p || !p->fpga_v1_caps || !p->fb)
        return;
    w = peek_dvd_settings(p->fb);
    if ((uint32_t)(w >> 32) != SET_MAGIC)
        return;
    raw = (unsigned)((w >> 8) & 0x1fu);
    trim = av_sync_raw_to_ms(raw);
    if (trim != p->osd_av_trim_ms) {
        p->osd_av_trim_ms = trim;
        fprintf(stderr, "A/V SYNC: trim %+d ms\n", trim);
    }
}

static void dvd_fpga_report_source(Player *p, DvdVideoStd std)
{
    unsigned src;

    if (!p || !p->fpga_v1_caps || !p->fb)
        return;
    src = dvd_std_to_fpga_src(std);
    if ((int)src == p->fpga_src_std)
        return;
    dvd_fpga_write_source(p->fb, src);
    p->fpga_src_std = (int)src;
    fprintf(stderr, "TV AUTO: source %s\n",
            src == FPGA_SRC_PAL ? "PAL" :
            src == FPGA_SRC_NTSC ? "NTSC" : "UNKNOWN");
}

static void *input_thread(void *opaque)
{
    Player *p = opaque;
    ControllerPad pad;
    int64_t t0 = av_gettime_relative();

    memset(&pad, 0, sizeof(pad));
    note_unpinned_cpu("input", &p->sched_input_cpu);
    fprintf(stderr, "DVD menu navigation: enabled\n");
    fprintf(stderr, "Hold CANCEL/B 3000 ms to return to launcher.\n");
    dbg("Controller: FPGA status 0x30400008  "
        "{DVD1, display_buf, joystick_0[30:0]}\n"
        "  bits: 0 Right  1 Left  2 Down  3 Up\n"
        "        4 Select/CONFIRM  5 Back/CANCEL  6 PLAY_PAUSE\n"
        "        7 MENU  8 PREVIOUS  9 NEXT   bit31=display_buf (ignored)\n"
        "  D-pad repeat after %.0f ms, every %.0f ms. Menu cmds queued "
        "for demux/dvdnav thread.\n",
        CTRL_REPEAT_DELAY_US / 1000.0, CTRL_REPEAT_RATE_US / 1000.0);

    while (!p->fail && !g_interrupt) {
        uint32_t bits = 0;
        int64_t now = av_gettime_relative();

        if (read_joystick_buttons(p->fb, &bits) == 0) {
            if (!pad.magic_ok) {
                pad.magic_ok = 1;
                fprintf(stderr, "Controller: ready\n");
            }
            controller_poll(&pad, bits, now, p);
        } else if (!pad.magic_logged) {
            pad.magic_logged = 1;
            dbg("Controller: waiting for DVD1 magic at 0x30400008\n");
        }
        dvd_fpga_poll_settings(p);
        av_usleep(CTRL_POLL_US);
    }

    p->ctrl_events = pad.events;
    p->input_cpu_us = av_gettime_relative() - t0;
    return NULL;
}

static void print_cpu_mask(unsigned mask)
{
    int i, n = 0;
    for (i = 0; i < 32; i++) {
        if (mask & (1u << i)) {
            fprintf(stderr, "%s%d", n ? ", " : "", i);
            n++;
        }
    }
    if (!n)
        fprintf(stderr, "(none)");
}

static int copy_codecpar(AVCodecParameters **dst, const AVCodecParameters *src)
{
    if (*dst)
        return 0;
    *dst = avcodec_parameters_alloc();
    if (!*dst)
        return -1;
    if (avcodec_parameters_copy(*dst, src) < 0) {
        avcodec_parameters_free(dst);
        return -1;
    }
    return 0;
}

static int replace_codecpar(AVCodecParameters **dst, const AVCodecParameters *src)
{
    avcodec_parameters_free(dst);
    return copy_codecpar(dst, src);
}

static int dvd_audio_ffmpeg_matches(const AVStream *st, int physical, int dvd_fmt)
{
    int sid, base = -1;

    if (!st || !st->codecpar ||
        st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return 0;
    if (physical < 0)
        return 0;
    sid = st->id & 0xff;
    if (sid == physical || (st->id & 0xffff) == physical)
        return 1;
    if (physical > 7)
        return 0;
    switch (dvd_fmt) {
    case DVD_AUDIO_FORMAT_AC3:
        base = 0x80;
        break;
    case DVD_AUDIO_FORMAT_DTS:
        base = 0x88;
        break;
    case DVD_AUDIO_FORMAT_LPCM:
        base = 0xa0;
        break;
    case DVD_AUDIO_FORMAT_MPEG:
    case DVD_AUDIO_FORMAT_MPEG2_EXT:
        base = 0xc0;
        break;
    default:
        break;
    }
    if (base >= 0 && sid == base + physical)
        return 1;
    if (dvd_fmt == DVD_AUDIO_FORMAT_AC3 && sid == 0xc0 + physical)
        return 1;
    if (dvd_fmt == DVD_AUDIO_FORMAT_DTS && sid == 0x98 + physical)
        return 1;
    if (dvd_fmt < 0 && sid >= 0x80 && sid <= 0xcf && (sid & 7) == physical)
        return 1;
    return 0;
}

/*
 * After DVDNAV_AUDIO_STREAM_CHANGE, bind FFmpeg to the VM's physical
 * stream. Flush audio packets only; do not touch video, nav_gen, or the
 * MrAudio clock origin.
 */
static void player_apply_audio_follow(Player *p)
{
    AVFormatContext *fmt;
    int i, found = -1, physical, dvd_fmt, flushed, logical;

    if (!p || p->audio_follow_physical < 0 || !p->avf)
        return;
    fmt = p->avf;
    physical = p->audio_follow_physical;
    dvd_fmt = p->audio_follow_fmt;
    logical = p->audio_follow_logical;
    for (i = 0; i < (int)fmt->nb_streams; i++) {
        if (dvd_audio_ffmpeg_matches(fmt->streams[i], physical, dvd_fmt)) {
            found = i;
            break;
        }
    }
    if (found < 0)
        return;
    fprintf(stderr, "AUDIO FOLLOW: logical=%d physical=%d ffmpeg_stream=#%d "
            "id=0x%x%s\n",
            logical, physical, found,
            fmt->streams[found]->id & 0xffff,
            found == p->ai ? " (already bound)" : "");
    p->audio_follow_physical = -1;
    p->audio_follow_logical = -1;
    p->audio_follow_fmt = -1;
    if (found == p->ai)
        return;
    if (replace_codecpar(&p->acp, fmt->streams[found]->codecpar) < 0) {
        fprintf(stderr, "AUDIO: follow failed (codecpar)\n");
        return;
    }
    p->ai = found;
    p->atb = fmt->streams[found]->time_base;
    flushed = pktq_flush(&p->aq);
    if (p->audio_started)
        p->audio_switch_req = 1;
    dbg("AUDIO: follow FFmpeg #%d id=0x%x %s  flushed=%d\n",
        found, fmt->streams[found]->id & 0xffff,
        avcodec_get_name(p->acp->codec_id), flushed);
}

static AVCodecContext *open_decoder(const AVCodecParameters *cp, AVRational tb,
                                    enum AVMediaType expect)
{
    const AVCodec *codec;
    AVCodecContext *ctx;
    int r;
    enum AVCodecID id = cp->codec_id;
    if (expect == AVMEDIA_TYPE_VIDEO && id == AV_CODEC_ID_NONE)
        id = AV_CODEC_ID_MPEG2VIDEO;
    codec = avcodec_find_decoder(id);
    if (!codec)
        return NULL;
    ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return NULL;
    if (avcodec_parameters_to_context(ctx, cp) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    if (ctx->codec_id == AV_CODEC_ID_NONE)
        ctx->codec_id = id;
    ctx->pkt_timebase = tb;
    r = avcodec_open2(ctx, codec, NULL);
    dbg("avcodec_open2(%s) returned %d\n", codec->name, r);
    if (r < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

static int emit_pcm(Player *p, MrAudio *mr, uint8_t *chunk, int *used,
                    const uint8_t *src, int remain)
{
    while (remain > 0) {
        int space = WRITE_CHUNK - *used;
        int take = remain < space ? remain : space;
        memcpy(chunk + *used, src, (size_t)take);
        *used += take;
        src += take;
        remain -= take;
        if (*used == WRITE_CHUNK) {
            mraudio_wait_fill(mr, WRITE_CHUNK);
            if (mraudio_write_all(mr, chunk, WRITE_CHUNK) < 0)
                return -1;
            mr->playing = 1;
            if (p)
                pause_note_resume_audio_write(p);
            *used = 0;
        }
    }
    return 0;
}

static void clock_wait_ready(Player *p)
{
    pthread_mutex_lock(&p->clock.mu);
    while (!p->clock.ready && !p->fail && !g_interrupt &&
           !p->in_menu && !p->still_active)
        pktq_wait_timeout(&p->clock.ready_cv, &p->clock.mu);
    pthread_mutex_unlock(&p->clock.mu);
}

static void prefill_release(Player *p, const char *reason)
{
    int got;

    got = buffered_queue_count(p);

    pthread_mutex_lock(&p->prefill_mu);
    if (!p->prefill_released) {
        p->prefill_released = 1;
        p->prefill_got = got;
        p->prefill_t1_us = av_gettime_relative();
        p->prefill_reason = reason ? reason : "released";
        pthread_cond_broadcast(&p->prefill_cv);
        dbg("video prefill gate released: %d / %d frames in %.3f s (%s)\n",
            p->prefill_got, p->prefill_req,
            p->prefill_t0_us
                ? (p->prefill_t1_us - p->prefill_t0_us) / 1e6 : 0.0,
            p->prefill_reason);
    }
    pthread_mutex_unlock(&p->prefill_mu);

    /* Wake audio if it is blocked in pktq_pop: the gate is not observed
     * from inside that wait. */
    pthread_mutex_lock(&p->aq.mu);
    pthread_cond_broadcast(&p->aq.not_empty);
    pthread_mutex_unlock(&p->aq.mu);
}

static int prefill_is_released(Player *p)
{
    int r;
    pthread_mutex_lock(&p->prefill_mu);
    r = p->prefill_released;
    pthread_mutex_unlock(&p->prefill_mu);
    return r;
}

static void prefill_wait(Player *p)
{
    pthread_mutex_lock(&p->prefill_mu);
    while (!p->prefill_released && !p->fail && !g_interrupt)
        pktq_wait_timeout(&p->prefill_cv, &p->prefill_mu);
    pthread_mutex_unlock(&p->prefill_mu);
}

static int pcm_hold_append(Player *p, const uint8_t *src, int n)
{
    int take;

    if (n <= 0)
        return 0;
    if (p->pcm_hold_len >= PCM_HOLD_START_BYTES) {
        p->pcm_hold_discarded += n;
        return 0;
    }
    take = n;
    if (p->pcm_hold_len + take > PCM_HOLD_START_BYTES) {
        take = PCM_HOLD_START_BYTES - p->pcm_hold_len;
        p->pcm_hold_discarded += (n - take);
    }
    if (p->pcm_hold_len + take > PCM_HOLD_MAX) {
        fprintf(stderr,
                "FAIL: audio PCM hold exceeded %d bytes during video prefill\n",
                PCM_HOLD_MAX);
        return -1;
    }
    if (p->pcm_hold_len + take > p->pcm_hold_cap) {
        int cap = p->pcm_hold_cap ? p->pcm_hold_cap * 2 : (BYTES_PER_SEC / 2);
        uint8_t *nb;
        while (cap < p->pcm_hold_len + take)
            cap *= 2;
        if (cap > PCM_HOLD_MAX)
            cap = PCM_HOLD_MAX;
        nb = av_realloc(p->pcm_hold, (size_t)cap);
        if (!nb)
            return -1;
        p->pcm_hold = nb;
        p->pcm_hold_cap = cap;
    }
    memcpy(p->pcm_hold + p->pcm_hold_len, src, (size_t)take);
    p->pcm_hold_len += take;
    return 0;
}

static int buffered_start_mraudio(Player *p, MrAudio *mr, uint8_t *chunk,
                                  int *chunk_used)
{
    int64_t hold_bytes;
    int64_t written0;

    if (p->buf_mraudio_started)
        return 0;

    hold_bytes = p->pcm_hold_len;
    p->pcm_hold_at_release = p->pcm_hold_len;
    dbg("audio gate observed release\n"
        "buffered PCM held before release: %" PRId64 " samples / %.1f ms"
        "  (%" PRId64 " bytes)\n"
        "PCM hold cap:                 %.1f ms  (%d bytes)\n"
        "PCM discarded/truncated:      %" PRId64 " samples / %.1f ms"
        "  (%" PRId64 " bytes)\n"
        "first audio PTS (clock origin): %s%.6f s\n"
        "starting MrAudio prime\n",
        hold_bytes / OUT_BYTES,
        hold_bytes * 1000.0 / (double)BYTES_PER_SEC,
        hold_bytes,
        PCM_HOLD_START_BYTES * 1000.0 / (double)BYTES_PER_SEC,
        PCM_HOLD_START_BYTES,
        p->pcm_hold_discarded / OUT_BYTES,
        p->pcm_hold_discarded * 1000.0 / (double)BYTES_PER_SEC,
        p->pcm_hold_discarded,
        p->first_audio_pts_us == AV_NOPTS_VALUE ? "(none) " : "",
        p->first_audio_pts_us == AV_NOPTS_VALUE
            ? 0.0 : p->first_audio_pts_us / 1e6);

    written0 = mr->bytes_written;
    if (mraudio_prime(mr) < 0)
        return -1;
    dbg("first MrAudio write bytes: %" PRId64 "  (prime)\n",
        mr->bytes_written - written0);

    if (p->pcm_hold_len > 0) {
        int flush_n = p->pcm_hold_len;

        if (flush_n > PCM_HOLD_START_BYTES) {
            p->pcm_hold_discarded += (flush_n - PCM_HOLD_START_BYTES);
            flush_n = PCM_HOLD_START_BYTES;
        }
        p->pcm_hold_used = flush_n;
        written0 = mr->bytes_written;
        if (emit_pcm(p, mr, chunk, chunk_used, p->pcm_hold, flush_n) < 0)
            return -1;
        dbg("MrAudio held-PCM flush bytes: %" PRId64
            "  (%.1f ms used for prime/fill)\n",
            mr->bytes_written - written0,
            flush_n * 1000.0 / (double)BYTES_PER_SEC);
        p->pcm_hold_len = 0;
    }

    if (mr->hw_pace)
        mraudio_poll(mr);
    if (mr->fill > 0)
        dbg("first nonzero MrAudio fill: %d bytes (%.1f ms)\n",
            mr->fill, mr->fill * 1000.0 / (double)BYTES_PER_SEC);
    else
        dbg("MrAudio fill after prime/flush: %d bytes\n", mr->fill);

    p->buf_mraudio_started = 1;
    dbg("MrAudio writes enabled — same prime/write loop as normal playback.\n");
    return 0;
}

/*
 * 1 = packet, 0 = eof/quit, 2 = prefill gate opened while queue was empty
 * (audio must start MrAudio immediately, then retry the pop).
 * 3 = audio_reset_req. 4 = pause drain/hold (do not consume a packet).
 */
static int pktq_pop_or_prefill_gate(PktQ *q, AVPacket *dst, Player *p)
{
    pthread_mutex_lock(&q->mu);
    for (;;) {
        if (pause_should_hold_audio(p)) {
            pthread_mutex_unlock(&q->mu);
            return 4;
        }
        if (q->count > 0)
            break;
        if (q->eof || q->quit || g_interrupt)
            break;
        if (p->audio_reset_req) {
            pthread_mutex_unlock(&q->mu);
            return 3;
        }
        if (p->audio_switch_req) {
            pthread_mutex_unlock(&q->mu);
            return 5;
        }
        pthread_mutex_unlock(&q->mu);
        if (player_buffered(p) && !p->buf_mraudio_started &&
            prefill_is_released(p) && !p->in_menu)
            return 2;
        pthread_mutex_lock(&q->mu);
        if (q->count > 0 || q->eof || q->quit || g_interrupt)
            continue;
        pktq_wait_timeout(&q->not_empty, &q->mu);
        if (p->audio_reset_req) {
            pthread_mutex_unlock(&q->mu);
            return 3;
        }
        if (p->audio_switch_req) {
            pthread_mutex_unlock(&q->mu);
            return 5;
        }
    }
    if (g_interrupt)
        q->quit = 1;
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mu);
        return 0;
    }
    av_packet_move_ref(dst, &q->pkts[q->head]);
    q->head = (q->head + 1) % q->cap;
    q->count--;
    q->popped++;
    pktq_note_depth(q);
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return 1;
}

static int buffered_try_start_mraudio(Player *p, MrAudio *mr, uint8_t *chunk,
                                     int *chunk_used, int64_t first_pts_us,
                                     unsigned clock_epoch)
{
    int ready;

    if (!player_buffered(p) || p->buf_mraudio_started)
        return 0;
    if (p->in_menu && first_pts_us == AV_NOPTS_VALUE)
        return 0;
    if (!prefill_is_released(p))
        return 0;
    if (buffered_start_mraudio(p, mr, chunk, chunk_used) < 0)
        return -1;
    if (mr->hw_pace)
        mraudio_poll(mr);
    ready = (first_pts_us != AV_NOPTS_VALUE && mr->fill >= READY_FILL);
    clock_publish(&p->clock, first_pts_us, mraudio_elapsed_us(mr),
                  p->last_audio_pts_us, mr->fill, mr->hw_pace, ready,
                  clock_epoch);
    if (ready)
        dbg("MrAudio clock ready  (fill=%d bytes, first_pts=%.3f s)\n",
            mr->fill, first_pts_us / 1e6);
    return 0;
}

static int buffered_emit_pcm(Player *p, MrAudio *mr, uint8_t *chunk,
                             int *chunk_used, const uint8_t *src, int remain)
{
    if (!prefill_is_released(p)) {
        if (pcm_hold_append(p, src, remain) < 0)
            return -1;
        return 0;
    }
    if (buffered_start_mraudio(p, mr, chunk, chunk_used) < 0)
        return -1;
    return emit_pcm(p, mr, chunk, chunk_used, src, remain);
}

static void *audio_thread(void *opaque)
{
    Player *p = opaque;
    int64_t t0 = av_gettime_relative();
    AVCodecContext *adec = NULL;
    SwrContext *swr = NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    MrAudio mr;
    uint8_t **out_data = NULL;
    int out_linesize = 0, out_cap = 0, chunk_used = 0;
    uint8_t chunk[WRITE_CHUNK];
    int64_t first_pts_us = AV_NOPTS_VALUE;
    unsigned clock_epoch = 0;

    note_unpinned_cpu("audio", &p->sched_audio_cpu);

    memset(&mr, 0, sizeof(mr));
    mr.wr_fd = -1;
    mr.rptr = mr.wptr = mr.fill = mr.comp = -1;

    stage = 7;
    if (!frame || !pkt) {
        player_abort(p);
        goto done;
    }
    if (mraudio_open(&mr) < 0) {
        player_abort(p);
        goto done;
    }
    if (!player_buffered(p)) {
        if (mraudio_prime(&mr) < 0) {
            player_abort(p);
            goto done;
        }
    } else {
        dbg("Buffered mode: delaying MrAudio prime/writes until video "
            "prefill (%d frames). PCM hold capped at %.1f ms.\n",
            p->prefill_req,
            PCM_HOLD_START_BYTES * 1000.0 / (double)BYTES_PER_SEC);
    }

    int clock_ready_logged = 0;
    int hold_full_logged = 0;
    clock_epoch = clock_epoch_now(&p->clock);

    adec = open_decoder(p->acp, p->atb, AVMEDIA_TYPE_AUDIO);
    if (!adec) {
        player_abort(p);
        goto done;
    }

    for (;;) {
        int got_pkt;

        if (p->audio_reset_req) {
            p->audio_reset_req = 0;
            if (adec)
                avcodec_flush_buffers(adec);
            first_pts_us = AV_NOPTS_VALUE;
            p->first_audio_pts_us = AV_NOPTS_VALUE;
            p->pcm_hold_len = 0;
            p->buf_mraudio_started = 0;
            mr.bytes_origin = mr.bytes_written - mr.prime_bytes;
            clock_epoch = clock_epoch_now(&p->clock);
            hold_full_logged = 0;
            clock_ready_logged = 0;
            dbg("DVD MENU: audio decoder/clock reset  gen=%u  "
                "origin_bytes=%" PRId64 "\n",
                player_nav_gen(p), mr.bytes_origin);
        }

        if (p->audio_switch_req) {
            AVCodecContext *nctx;

            p->audio_switch_req = 0;
            p->pcm_hold_len = 0;
            if (adec)
                avcodec_free_context(&adec);
            if (swr)
                swr_free(&swr);
            nctx = p->acp ? open_decoder(p->acp, p->atb, AVMEDIA_TYPE_AUDIO)
                          : NULL;
            if (!nctx) {
                fprintf(stderr, "AUDIO: decoder reopen failed\n");
                player_abort(p);
                goto done;
            }
            adec = nctx;
            dbg("AUDIO: decoder reopened for stream change (clock kept)\n");
        }

        if (pause_should_hold_audio(p)) {
            pause_service_audio(p, &mr, first_pts_us, clock_epoch);
            if (p->fail || g_interrupt)
                break;
            continue;
        }

        if (player_buffered(p)) {
            if (buffered_try_start_mraudio(p, &mr, chunk, &chunk_used,
                                           first_pts_us, clock_epoch) < 0) {
                player_abort(p);
                goto done;
            }
            if (!p->buf_mraudio_started &&
                p->pcm_hold_len >= PCM_HOLD_START_BYTES &&
                !prefill_is_released(p)) {
                if (!hold_full_logged) {
                    hold_full_logged = 1;
                    dbg("audio PCM hold at cap (%.1f ms); waiting for "
                        "video prefill, not decoding more\n",
                        p->pcm_hold_len * 1000.0 / (double)BYTES_PER_SEC);
                }
                prefill_wait(p);
                if (p->fail || g_interrupt)
                    break;
                continue;
            }
            got_pkt = pktq_pop_or_prefill_gate(&p->aq, pkt, p);
            if (got_pkt == 2 || got_pkt == 3 || got_pkt == 5)
                continue;
            if (got_pkt == 4) {
                pause_service_audio(p, &mr, first_pts_us, clock_epoch);
                if (p->fail || g_interrupt)
                    break;
                continue;
            }
            if (got_pkt == 0)
                break;
        } else {
            got_pkt = pktq_pop_or_prefill_gate(&p->aq, pkt, p);
            if (got_pkt == 2 || got_pkt == 3 || got_pkt == 5)
                continue;
            if (got_pkt == 4) {
                pause_service_audio(p, &mr, first_pts_us, clock_epoch);
                if (p->fail || g_interrupt)
                    break;
                continue;
            }
            if (got_pkt == 0)
                break;
        }
        p->audio_packets++;
        if (avcodec_send_packet(adec, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(adec, frame) == 0) {
            p->audio_frames++;
            p->src_samples += frame->nb_samples;
            if (frame->pts == AV_NOPTS_VALUE) {
                p->audio_missing_pts++;
            } else {
                int64_t pus = tb_to_us(p->atb, frame->pts);
                if (first_pts_us == AV_NOPTS_VALUE && pus != AV_NOPTS_VALUE) {
                    first_pts_us = pus;
                    p->first_audio_pts_us = pus;
                    dbg("First decoded audio PTS: %.6f s (clock origin)\n",
                        pus / 1e6);
                }
                if (p->last_audio_pts_us != AV_NOPTS_VALUE &&
                    pus != AV_NOPTS_VALUE) {
                    int64_t gap = pus - p->last_audio_pts_us;
                    if (gap < 0)
                        gap = -gap;
                    if (gap > DISC_JUMP_US)
                        p->audio_disc++;
                }
                p->last_audio_pts_us = pus;
            }
            if (!swr) {
                AVChannelLayout out_layout;
                av_channel_layout_default(&out_layout, OUT_CHANNELS);
                if (swr_alloc_set_opts2(
                        &swr, &out_layout, AV_SAMPLE_FMT_S16, OUT_RATE,
                        &frame->ch_layout, (enum AVSampleFormat)frame->format,
                        frame->sample_rate, 0, NULL) < 0 ||
                    !swr || swr_init(swr) < 0) {
                    fprintf(stderr, "swr init failed\n");
                    player_abort(p);
                    av_channel_layout_uninit(&out_layout);
                    av_frame_unref(frame);
                    goto done;
                }
                av_channel_layout_uninit(&out_layout);
                dbg("Audio convert: %s %d Hz %d ch -> 48 kHz S16 stereo\n",
                    av_get_sample_fmt_name(frame->format)
                        ? av_get_sample_fmt_name(frame->format) : "?",
                    frame->sample_rate, frame->ch_layout.nb_channels);
            }
            {
                int max_out = (int)av_rescale_rnd(
                    swr_get_delay(swr, frame->sample_rate) + frame->nb_samples,
                    OUT_RATE, frame->sample_rate, AV_ROUND_UP);
                if (max_out < 32)
                    max_out = 32;
                if (max_out > out_cap) {
                    if (out_data)
                        av_freep(&out_data[0]);
                    av_freep(&out_data);
                    if (av_samples_alloc_array_and_samples(
                            &out_data, &out_linesize, OUT_CHANNELS, max_out,
                            AV_SAMPLE_FMT_S16, 0) < 0) {
                        player_abort(p);
                        goto done;
                    }
                    out_cap = max_out;
                }
                int got = swr_convert(swr, out_data, max_out,
                                      (const uint8_t **)frame->extended_data,
                                      frame->nb_samples);
                if (got > 0) {
                    p->out_samples += got;
                    if (player_buffered(p)) {
                        if (buffered_emit_pcm(p, &mr, chunk, &chunk_used,
                                              out_data[0],
                                              got * OUT_BYTES) < 0) {
                            player_abort(p);
                            av_frame_unref(frame);
                            goto done;
                        }
                    } else if (emit_pcm(p, &mr, chunk, &chunk_used, out_data[0],
                                        got * OUT_BYTES) < 0) {
                        player_abort(p);
                        av_frame_unref(frame);
                        goto done;
                    }
                }
            }
            if (mr.hw_pace)
                mraudio_poll(&mr);
            p->live_underruns = mr.underruns;
            {
                int64_t elapsed = mraudio_elapsed_us(&mr);
                int ready = (first_pts_us != AV_NOPTS_VALUE &&
                             mr.fill >= READY_FILL);
                if (player_buffered(p) && !p->buf_mraudio_started)
                    ready = 0;
                clock_publish(&p->clock, first_pts_us, elapsed,
                              p->last_audio_pts_us, mr.fill, mr.hw_pace, ready,
                              clock_epoch);
                if (player_buffered(p) && ready && !clock_ready_logged) {
                    clock_ready_logged = 1;
                    dbg("MrAudio clock ready  (fill=%d bytes, "
                        "first_pts=%.3f s)\n",
                        mr.fill, first_pts_us / 1e6);
                }
            }
            av_frame_unref(frame);
        }
    }

    if (player_buffered(p) && !p->buf_mraudio_started) {
        prefill_wait(p);
        if (!p->fail && !g_interrupt) {
            if (buffered_start_mraudio(p, &mr, chunk, &chunk_used) < 0)
                player_abort(p);
        }
    }

    if (chunk_used > 0) {
        while (chunk_used & 3)
            chunk[chunk_used++] = 0;
        mraudio_wait_fill(&mr, chunk_used);
        if (mraudio_write_all(&mr, chunk, chunk_used) == 0)
            pause_note_resume_audio_write(p);
        mr.playing = 1;
    }
    mraudio_drain(&mr);
    clock_publish(&p->clock, first_pts_us, mraudio_elapsed_us(&mr),
                  p->last_audio_pts_us, mr.fill, mr.hw_pace,
                  first_pts_us != AV_NOPTS_VALUE, clock_epoch);

    dbg("Audio thread done: packets=%lu frames=%lu samples=%" PRId64
        " underruns=%d (%.3f s) fill min/avg/max=%" PRId64 "/%.0f/%" PRId64 "\n",
        p->audio_packets, p->audio_frames, p->out_samples,
        mr.underruns, mr.underrun_us / 1e6,
        mr.fill_min,
        mr.fill_n ? (double)mr.fill_sum / mr.fill_n : 0.0,
        mr.fill_max);

done:
    p->mr_stats = mr;
    p->audio_cpu_us = av_gettime_relative() - t0;
    if (out_data)
        av_freep(&out_data[0]);
    av_freep(&out_data);
    swr_free(&swr);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&adec);
    mraudio_close(&mr);
    return NULL;
}

static AVRational detect_fps(const Player *p, const AVCodecContext *vdec)
{
    if (vdec && vdec->framerate.num > 0 && vdec->framerate.den > 0)
        return vdec->framerate;
    if (vdec && vdec->time_base.num > 0 && vdec->time_base.den > 0 &&
        vdec->ticks_per_frame > 0)
        return av_make_q(vdec->time_base.den,
                         vdec->time_base.num * vdec->ticks_per_frame);
    if (p->avg_fr.num > 0 && p->avg_fr.den > 0)
        return p->avg_fr;
    if (p->r_fr.num > 0 && p->r_fr.den > 0)
        return p->r_fr;
    return (AVRational){0, 0};
}

static int64_t frame_duration_tb(const Player *p)
{
    if (p->fps.num > 0 && p->fps.den > 0 &&
        p->vtb.num > 0 && p->vtb.den > 0)
        return av_rescale_q(1, av_inv_q(p->fps), p->vtb);
    return 0;
}

static void assign_video_pts(Player *p, const AVFrame *frame,
                             const AVCodecContext *vdec)
{
    AVRational detected = detect_fps(p, vdec);
    if (detected.num > 0 && detected.den > 0)
        p->fps = detected;
    const int64_t genuine =
        (frame->pts != AV_NOPTS_VALUE) ? frame->pts : AV_NOPTS_VALUE;
    const int64_t dur = frame_duration_tb(p);
    int64_t timeline = AV_NOPTS_VALUE;
    if (genuine != AV_NOPTS_VALUE && p->vtb.num > 0 && p->vtb.den > 0) {
        p->genuine_pts_count++;
        if (p->first_genuine_pts == AV_NOPTS_VALUE) {
            p->first_genuine_pts = genuine;
            p->timeline_pts = genuine;
            timeline = genuine;
        } else if (dur > 0) {
            int64_t expected = p->timeline_pts + dur;
            int64_t disc = genuine - expected;
            timeline = expected;
            if (disc > dur)
                disc = dur;
            else if (disc < -dur)
                disc = -dur;
            p->timeline_pts = expected + disc;
        } else {
            timeline = genuine;
            p->timeline_pts = genuine;
        }
    } else if (p->first_genuine_pts != AV_NOPTS_VALUE && dur > 0) {
        p->interpolated_count++;
        timeline = p->timeline_pts + dur;
        p->timeline_pts = timeline;
    } else {
        p->fallback_frames++;
    }
    p->assigned_pts = timeline;
}

/* Diagnostics and stale recovery. Source frame duration from detected fps. */
static int64_t frame_duration_us(const Player *p)
{
    if (p->fps.num > 0 && p->fps.den > 0)
        return av_rescale_rnd(1000000LL, (int64_t)p->fps.den,
                              (int64_t)p->fps.num, AV_ROUND_NEAR_INF);
    return 0;
}

static int player_active_h(const Player *p)
{
    if (p && p->video_h > 0)
        return p->video_h;
    return FB_H;
}

static void log_yuv_frame_meta_once(Player *p, const AVFrame *frame)
{
    const char *cl = NULL, *cr = NULL, *cs = NULL, *fmt = NULL;

    if (!p || !frame || p->yuv_meta_logged)
        return;
    p->yuv_meta_logged = 1;
    fmt = av_get_pix_fmt_name(frame->format);
    cl = av_chroma_location_name(frame->chroma_location);
    cr = av_color_range_name(frame->color_range);
    cs = av_color_space_name(frame->colorspace);
    fprintf(stderr,
            "FPGA YUV420 META: interlaced_frame=%d top_field_first=%d "
            "chroma_location=%s (%d) color_range=%s colorspace=%s "
            "format=%s %dx%d linesize=%d/%d/%d\n"
            "FPGA YUV420 CHROMA: SIMPLE chroma_row=y>>1 "
            "(approximate for interlaced MPEG-2 4:2:0; not final)\n",
            frame->interlaced_frame, frame->top_field_first,
            cl ? cl : "unspecified", (int)frame->chroma_location,
            cr ? cr : "unspecified", cs ? cs : "unspecified",
            fmt ? fmt : "?", frame->width, frame->height,
            frame->linesize[0], frame->linesize[1], frame->linesize[2]);
}

static int copy_yuv420_to_slot(uint8_t *slot, const AVFrame *frame, int active_h)
{
    int y, h, ch, ycopy, ccopy;

    if (!slot || !frame || !frame->data[0] || !frame->data[1] || !frame->data[2])
        return -1;
    if (frame->format != AV_PIX_FMT_YUV420P &&
        frame->format != AV_PIX_FMT_YUVJ420P)
        return -1;
    h = active_h;
    if (frame->height > 0 && frame->height < h)
        h = frame->height;
    if (h < 2)
        return -1;
    if (h > FB_H)
        h = FB_H;
    h &= ~1;
    ch = h / 2;
    ycopy = FB_W;
    ccopy = YUV_C_STRIDE;
    if (frame->width > 0 && frame->width < ycopy)
        ycopy = frame->width;
    if (ycopy > YUV_Y_STRIDE)
        ycopy = YUV_Y_STRIDE;
    ccopy = ycopy / 2;
    if (frame->linesize[0] < ycopy || frame->linesize[1] < ccopy ||
        frame->linesize[2] < ccopy)
        return -1;
    for (y = 0; y < h; y++)
        memcpy(slot + YUV_Y_OFF + (size_t)y * YUV_Y_STRIDE,
               frame->data[0] + (size_t)y * (size_t)frame->linesize[0],
               (size_t)ycopy);
    for (y = 0; y < ch; y++) {
        memcpy(slot + YUV_U_OFF + (size_t)y * YUV_C_STRIDE,
               frame->data[1] + (size_t)y * (size_t)frame->linesize[1],
               (size_t)ccopy);
        memcpy(slot + YUV_V_OFF + (size_t)y * YUV_C_STRIDE,
               frame->data[2] + (size_t)y * (size_t)frame->linesize[2],
               (size_t)ccopy);
    }
    return 0;
}

static void yuv_copy_note(Player *p, int64_t us)
{
    if (!p || !p->yuv_copy)
        return;
    if (us < 0)
        us = 0;
    if (p->yuv_copy_n < ISO_SAMPLE_CAP)
        p->yuv_copy[p->yuv_copy_n] = us;
    p->yuv_copy_sum += us;
    p->yuv_copy_n++;
    if (us > p->yuv_copy_max)
        p->yuv_copy_max = us;
}

static size_t player_active_fb_bytes(const Player *p)
{
    return (size_t)FB_STRIDE * (size_t)player_active_h(p);
}

static const char *dvd_std_name(DvdVideoStd std)
{
    switch (std) {
    case DVD_VIDEO_PAL:  return "PAL";
    case DVD_VIDEO_NTSC: return "NTSC";
    default:             return "unknown";
    }
}

static int fps_is_pal(AVRational fps)
{
    double f;

    if (fps.num <= 0 || fps.den <= 0)
        return 1;
    f = av_q2d(fps);
    return f >= 24.5 && f <= 25.5;
}

static int fps_is_ntsc(AVRational fps)
{
    double f;

    if (fps.num <= 0 || fps.den <= 0)
        return 1;
    f = av_q2d(fps);
    return f >= 29.5 && f <= 30.5;
}

/* Explicit DVD-standard check. DDR stays 720x576-backed; NTSC uses 480 lines. */
static int player_accept_video_frame(Player *p, const AVFrame *frame,
                                     const AVCodecContext *vdec)
{
    AVRational fps;
    DvdVideoStd std = DVD_VIDEO_UNKNOWN;
    int h;

    if (!p || !frame) {
        fprintf(stderr, "FAIL: missing video frame\n");
        if (p)
            player_abort(p);
        return -1;
    }
    fps = detect_fps(p, vdec);
    if (fps.num > 0 && fps.den > 0)
        p->fps = fps;

    if (frame->width == FB_W && frame->height == FB_H_PAL && fps_is_pal(p->fps))
        std = DVD_VIDEO_PAL;
    else if (frame->width == FB_W && frame->height == FB_H_NTSC &&
             fps_is_ntsc(p->fps))
        std = DVD_VIDEO_NTSC;

    if (std == DVD_VIDEO_UNKNOWN) {
        fprintf(stderr,
                "FAIL: unsupported DVD video %dx%d @ %.3f fps "
                "(supported: 720x576 @ 25 PAL, 720x480 @ 29.97 NTSC)\n",
                frame->width, frame->height,
                (p->fps.num > 0 && p->fps.den > 0) ? av_q2d(p->fps) : 0.0);
        player_abort(p);
        return -1;
    }

    h = (std == DVD_VIDEO_NTSC) ? FB_H_NTSC : FB_H_PAL;
    if (p->dvd_std == DVD_VIDEO_UNKNOWN) {
        p->dvd_std = std;
        p->video_w = FB_W;
        p->video_h = h;
        fprintf(stderr, "DVD video standard: %s %dx%d @ %.3f fps\n",
                dvd_std_name(std), FB_W, h,
                (p->fps.num > 0 && p->fps.den > 0) ? av_q2d(p->fps) : 0.0);
        dbg("DVD video: active %dx%d  DDR backing %dx%d stride %d  "
            "T=%.3f ms  fps %d/%d\n",
            FB_W, h, FB_W, FB_H, FB_STRIDE,
            frame_duration_us(p) / 1000.0, p->fps.num, p->fps.den);
        dvd_fpga_report_source(p, std);
    } else if (p->dvd_std != std || frame->width != p->video_w ||
               frame->height != p->video_h) {
        fprintf(stderr,
                "DVD video standard changed to %s %dx%d @ %.3f fps "
                "(was %s %dx%d) — keeping navigation\n",
                dvd_std_name(std), frame->width, frame->height,
                (p->fps.num > 0 && p->fps.den > 0) ? av_q2d(p->fps) : 0.0,
                dvd_std_name(p->dvd_std), p->video_w, p->video_h);
        p->dvd_std = std;
        p->video_w = FB_W;
        p->video_h = h;
        dvd_fpga_report_source(p, std);
    }
    return 0;
}

/* Permanent content-phase: presentation_vpts = raw_vpts - N*T. Raw PTS is
 * never modified. N=0 → identity. */
static int64_t presentation_phase_us(const Player *p)
{
    int64_t T;
    if (p->initial_skip_req <= 0)
        return 0;
    T = frame_duration_us(p);
    if (T <= 0)
        return 0;
    return (int64_t)p->initial_skip_req * T;
}

/* Temporary --video-advance-ms: buffered-YUV only. Default path is 0.
 * OSD A/V Sync trim is subtracted so:
 *   +OSD ms = present VIDEO later relative to audio
 *   -OSD ms = present VIDEO earlier relative to audio
 * OSD 0 ms is identical to the --video-advance-ms baseline (launcher: 20).
 * MrAudio consumed bytes remain the sole media clock. */
static int64_t video_advance_applied_us(const Player *p)
{
    int64_t base = 0;

    if (p->buffered_yuv && p->video_advance_ms > 0)
        base = (int64_t)p->video_advance_ms * 1000;
    return base - (int64_t)p->osd_av_trim_ms * 1000;
}

static int64_t total_presentation_phase_us(const Player *p)
{
    return presentation_phase_us(p) + video_advance_applied_us(p);
}

static int64_t presentation_vpts_from_raw(const Player *p, int64_t raw_vpts_us)
{
    if (raw_vpts_us == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;
    return raw_vpts_us - total_presentation_phase_us(p);
}

/* Diagnostics only. Not used to hold, mailbox, or choose a buffer. */
static int64_t diag_native_period_us(const Player *p)
{
    return frame_duration_us(p);
}

static int64_t thread_cpu_us(void)
{
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
#else
    return -1;
#endif
}

static void stat_add(unsigned long *n, int64_t *sum, int64_t *minv, int64_t *maxv,
                     int64_t v)
{
    if (v < 0)
        v = 0;
    if (!*n || v < *minv)
        *minv = v;
    if (!*n || v > *maxv)
        *maxv = v;
    *sum += v;
    (*n)++;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t da = *(const int64_t *)a;
    int64_t db = *(const int64_t *)b;

    return (da > db) - (da < db);
}

static int64_t sorted_pct(const int64_t *v, int n, int pct)
{
    long idx;

    if (n <= 0)
        return 0;
    idx = ((long)pct * (n - 1) + 50) / 100;
    if (idx < 0)
        idx = 0;
    if (idx >= n)
        idx = n - 1;
    return v[idx];
}

static void subperf_push(int64_t *arr, int *n, int64_t *sum, int64_t v)
{
    if (arr && *n < ISO_SAMPLE_CAP)
        arr[*n] = v;
    *sum += v;
    (*n)++;
}

static void subperf_note_present(Player *p, int sub_active, int64_t blend_us,
                                 int64_t sws_us, int64_t ack_us, int ack_waited,
                                 int64_t cycle_us)
{
    p->subperf.last_active = sub_active;
    if (sub_active) {
        p->subperf.act_frames++;
        if (blend_us < 0)
            blend_us = 0;
        if (blend_us > p->subperf.act_blend_max)
            p->subperf.act_blend_max = blend_us;
        subperf_push(p->subperf.act_blend, &p->subperf.act_blend_n,
                     &p->subperf.act_blend_sum, blend_us);
        subperf_push(p->subperf.act_sws, &p->subperf.act_sws_n,
                     &p->subperf.act_sws_sum, sws_us);
        subperf_push(p->subperf.act_sws_sub, &p->subperf.act_combo_n,
                     &p->subperf.act_combo_sum, sws_us + blend_us);
        subperf_push(p->subperf.act_cyc, &p->subperf.act_cyc_n,
                     &p->subperf.act_cyc_sum, cycle_us);
        if (ack_waited)
            subperf_push(p->subperf.act_ack, &p->subperf.act_ack_n,
                         &p->subperf.act_ack_sum, ack_us);
    } else {
        p->subperf.inact_frames++;
        subperf_push(p->subperf.inact_sws, &p->subperf.inact_sws_n,
                     &p->subperf.inact_sws_sum, sws_us);
        subperf_push(p->subperf.inact_cyc, &p->subperf.inact_cyc_n,
                     &p->subperf.inact_cyc_sum, cycle_us);
        if (ack_waited)
            subperf_push(p->subperf.inact_ack, &p->subperf.inact_ack_n,
                         &p->subperf.inact_ack_sum, ack_us);
    }
}

static void iso_note_present(Player *p, int64_t ack_us, int ack_waited,
                             int64_t cycle_us)
{
    if (!p->iso_started || !p->iso_ack || !p->iso_cyc)
        return;
    if (cycle_us < 0)
        cycle_us = 0;
    if (p->iso_n < ISO_SAMPLE_CAP)
        p->iso_cyc[p->iso_n] = cycle_us;
    p->iso_cyc_sum += cycle_us;
    p->iso_n++;
    if (ack_waited && ack_us >= 0) {
        if (p->iso_ack_n < ISO_SAMPLE_CAP)
            p->iso_ack[p->iso_ack_n] = ack_us;
        p->iso_ack_sum += ack_us;
        p->iso_ack_n++;
        if (ack_us > 20000)
            p->iso_ack_gt20++;
        if (ack_us > 30000)
            p->iso_ack_gt30++;
        if (ack_us > 40000)
            p->iso_ack_gt40++;
    }
}

static void iso_finish_warmup_or_record(Player *p, int64_t ack_us,
                                        int ack_waited, int64_t cycle_us)
{
    if (!p->perf_present_no_convert)
        return;
    if (p->iso_warm_presents < ISO_WARM_PRESENTS) {
        p->iso_warm_presents++;
        if (p->iso_warm_presents == ISO_WARM_PRESENTS) {
            p->iso_t0_us = av_gettime_relative();
            p->iso_decoded0 = p->video_decoded;
            p->iso_stale0 = p->stale_dropped;
            p->iso_started = 1;
            fprintf(stderr,
                    "PERF isolation ACK window started "
                    "(A/B primed, per-frame sws_scale off).\n");
        }
        return;
    }
    iso_note_present(p, ack_us, ack_waited, cycle_us);
}

static void phase_sws_enter(Player *p)
{
    if (!p->phase_decode)
        return;
    pthread_mutex_lock(&p->phase_mu);
    p->sws_busy = 1;
    pthread_mutex_unlock(&p->phase_mu);
    if (__atomic_load_n(&p->producer_in_decode, __ATOMIC_ACQUIRE))
        p->phase_overlap_n++;
}

static void phase_sws_leave(Player *p)
{
    if (!p->phase_decode)
        return;
    pthread_mutex_lock(&p->phase_mu);
    p->sws_busy = 0;
    pthread_cond_broadcast(&p->phase_cv);
    pthread_mutex_unlock(&p->phase_mu);
}

static void phase_wait_for_sws_idle(Player *p)
{
    int waited = 0;

    if (!p->phase_decode || p->in_menu || !p->buf_playing)
        return;
    if (!prefill_is_released(p))
        return;
    for (;;) {
        int yuv_n, vq_n, busy;

        if (p->fail || g_interrupt)
            return;
        yuv_n = yuvring_count(&p->yuvring);
        vq_n = pktq_count(&p->vq);
        if (yuv_n <= PHASE_YUV_LOW_WATER) {
            if (waited)
                p->phase_bypass_low++;
            return;
        }
        if (vq_n >= PHASE_VQ_HIGH_WATER) {
            p->phase_bypass_vq++;
            return;
        }
        pthread_mutex_lock(&p->phase_mu);
        busy = p->sws_busy;
        if (!busy || p->fail || g_interrupt) {
            pthread_mutex_unlock(&p->phase_mu);
            return;
        }
        if (!waited) {
            p->phase_wait_n++;
            waited = 1;
        }
        pktq_wait_timeout(&p->phase_cv, &p->phase_mu);
        pthread_mutex_unlock(&p->phase_mu);
    }
}

static void phase_decode_begin(Player *p)
{
    int busy;

    if (!p->phase_decode)
        return;
    phase_wait_for_sws_idle(p);
    pthread_mutex_lock(&p->phase_mu);
    busy = p->sws_busy;
    pthread_mutex_unlock(&p->phase_mu);
    if (busy)
        p->phase_overlap_n++;
    __atomic_store_n(&p->producer_in_decode, 1, __ATOMIC_RELEASE);
}

static void phase_decode_end(Player *p)
{
    if (!p->phase_decode)
        return;
    __atomic_store_n(&p->producer_in_decode, 0, __ATOMIC_RELEASE);
}

static void phase_note_present(Player *p, int64_t sws_us, int64_t ack_us,
                               int ack_waited, int64_t cycle_us)
{
    if (!p->phase_decode)
        return;
    if (sws_us < 0)
        sws_us = 0;
    if (cycle_us < 0)
        cycle_us = 0;
    if (p->phase_sws && p->phase_sws_n < ISO_SAMPLE_CAP)
        p->phase_sws[p->phase_sws_n] = sws_us;
    p->phase_sws_sum += sws_us;
    p->phase_sws_n++;
    if (ack_waited && ack_us >= 0) {
        if (p->phase_ack && p->phase_ack_n < ISO_SAMPLE_CAP)
            p->phase_ack[p->phase_ack_n] = ack_us;
        p->phase_ack_sum += ack_us;
        p->phase_ack_n++;
    }
    if (p->phase_cyc && p->phase_cyc_n < ISO_SAMPLE_CAP)
        p->phase_cyc[p->phase_cyc_n] = cycle_us;
    p->phase_cyc_sum += cycle_us;
    p->phase_cyc_n++;
}

static void print_miss_rec(const Player *p, unsigned miss_n, int frame,
                           int64_t vpts_us, int64_t av_off_us, int64_t ack_iv_us,
                           int64_t ack_wait_us, int ack_instant,
                           int64_t decode_us, int64_t sws_wall_us,
                           int64_t sws_cpu_us, int64_t preempt_us,
                           int64_t ack_to_mbox_us, int64_t conv_to_mbox_us,
                           int64_t mbox_to_ack_us, int64_t cycle_us)
{
    if (!g_debug_stats)
        return;
    fprintf(stderr,
            "MISS #%u frame=%d vpts=%.3fs av=%+.1fms ack_iv=%.1fms "
            "ack_wait=%.1fms%s decode=%.1fms %s_wall=%.1fms ",
            miss_n, frame,
            vpts_us == AV_NOPTS_VALUE ? 0.0 : vpts_us / 1e6,
            av_off_us / 1000.0, ack_iv_us / 1000.0, ack_wait_us / 1000.0,
            ack_instant ? "(inst)" : "",
            decode_us >= 0 ? decode_us / 1000.0 : -1.0,
            p->buffered_video ? "memcpy" : "sws",
            sws_wall_us / 1000.0);
    if (p->sws_cpu_ok && sws_cpu_us >= 0)
        fprintf(stderr, "%s_cpu=%.1fms preempt=%.1fms ",
                p->buffered_video ? "memcpy" : "sws",
                sws_cpu_us / 1000.0, preempt_us / 1000.0);
    else
        fprintf(stderr, "sws_cpu=n/a preempt=n/a ");
    fprintf(stderr,
            "ack_to_mbox=%.1fms conv_to_mbox=%.1fms mbox_to_ack=%.1fms "
            "cycle=%.1fms\n",
            ack_to_mbox_us / 1000.0, conv_to_mbox_us / 1000.0,
            mbox_to_ack_us / 1000.0, cycle_us / 1000.0);
}

static void record_boundary_miss(Player *p, int64_t ack_iv_us)
{
    unsigned n;
    int frame = p->last_path.valid ? p->last_path.frame : p->rendered;
    int64_t vpts = p->last_path.valid ? p->last_path.vpts_us : AV_NOPTS_VALUE;
    int64_t av_off = p->last_path.valid ? p->last_path.av_off_us : 0;
    int64_t ack_wait = p->last_path.valid ? p->last_path.ack_wait_us : 0;
    int inst = p->last_path.valid ? p->last_path.ack_instant : 0;
    int64_t decode = p->last_path.valid ? p->last_path.decode_us : -1;
    int64_t sws_wall = p->last_path.valid ? p->last_path.sws_wall_us : 0;
    int64_t sws_cpu = p->last_path.valid ? p->last_path.sws_cpu_us : -1;
    int64_t preempt = p->last_path.valid ? p->last_path.preempt_us : -1;
    int64_t a2m = p->last_path.valid ? p->last_path.ack_to_mbox_us : 0;
    int64_t c2m = p->last_path.valid ? p->last_path.conv_to_mbox_us : 0;
    int64_t cyc = p->last_path.valid ? p->last_path.cycle_us : 0;
    int64_t m2a = p->last_mbox_to_ack_us;

    p->miss_total++;
    n = p->miss_total;
    if (p->subperf.last_active)
        p->subperf.act_miss++;
    else
        p->subperf.inact_miss++;

    p->misses[p->miss_log_i].miss_n = n;
    p->misses[p->miss_log_i].frame = frame;
    p->misses[p->miss_log_i].vpts_us = vpts;
    p->misses[p->miss_log_i].av_off_us = av_off;
    p->misses[p->miss_log_i].ack_iv_us = ack_iv_us;
    p->misses[p->miss_log_i].ack_wait_us = ack_wait;
    p->misses[p->miss_log_i].ack_instant = inst;
    p->misses[p->miss_log_i].decode_us = decode;
    p->misses[p->miss_log_i].sws_wall_us = sws_wall;
    p->misses[p->miss_log_i].sws_cpu_us = sws_cpu;
    p->misses[p->miss_log_i].preempt_us = preempt;
    p->misses[p->miss_log_i].ack_to_mbox_us = a2m;
    p->misses[p->miss_log_i].conv_to_mbox_us = c2m;
    p->misses[p->miss_log_i].mbox_to_ack_us = m2a;
    p->misses[p->miss_log_i].cycle_us = cyc;
    p->miss_log_i = (p->miss_log_i + 1) % MISS_LOG_CAP;
    if (p->miss_log_n < MISS_LOG_CAP)
        p->miss_log_n++;
}

static void note_display_ack(Player *p, int64_t now_us)
{
    int64_t period = diag_native_period_us(p);

    p->diag_period_us = period;
    if (p->last_mbox_wall_us > 0) {
        int64_t lat = now_us - p->last_mbox_wall_us;
        if (lat < 0)
            lat = 0;
        if (!p->mbox_to_ack_n || lat < p->mbox_to_ack_min)
            p->mbox_to_ack_min = lat;
        if (lat > p->mbox_to_ack_max)
            p->mbox_to_ack_max = lat;
        p->mbox_to_ack_sum += lat;
        p->mbox_to_ack_n++;
        p->last_mbox_to_ack_us = lat;
        if (period > 0) {
            if (lat < period)
                p->mbox_to_ack_0_1t++;
            else if (lat < 2 * period)
                p->mbox_to_ack_1_2t++;
            else if (lat < 3 * period)
                p->mbox_to_ack_2_3t++;
            else
                p->mbox_to_ack_gt3t++;
        }
    }
    if (p->last_ack_wall_us > 0) {
        int64_t iv = now_us - p->last_ack_wall_us;
        if (iv < 0)
            iv = 0;
        if (!p->ack_interval_n || iv < p->ack_interval_min)
            p->ack_interval_min = iv;
        if (iv > p->ack_interval_max)
            p->ack_interval_max = iv;
        p->ack_interval_sum += iv;
        p->ack_interval_n++;
        p->last_ack_interval_us = iv;
        if (period > 0) {
            int64_t half = period / 2;
            int64_t nper = (iv + half) / period;
            int64_t missed = (nper > 1) ? nper - 1 : 0;
            p->ack_missed_periods += missed;
            if (iv < period + half)
                p->ack_normal++;
            else if (iv < 2 * period + half)
                p->ack_miss1++;
            else if (iv < 3 * period + half)
                p->ack_miss2++;
            else
                p->ack_miss_gt2++;
            if (missed > 0)
                record_boundary_miss(p, iv);
            if (p->ack_interval_n <= OFFSET_LOG_N) {
                dbg("ack[%lu]: interval=%.1fms  (~%.2f T)  missed=%"
                    PRId64 "  mbox->ack=%.1fms  T=%.3fms\n",
                    p->ack_interval_n, iv / 1000.0,
                    period ? (double)iv / (double)period : 0.0,
                    missed, p->last_mbox_to_ack_us / 1000.0,
                    period / 1000.0);
            }
        }
    }
    p->last_ack_wall_us = now_us;
    p->ack_n++;
}

static int wait_valid_display_buf(Player *p, int *out, int64_t timeout_us)
{
    int64_t t0 = av_gettime_relative();

    for (;;) {
        if (read_display_buf(p->fb, out) == 0)
            return 0;
        if (p->fail || g_interrupt)
            return -1;
        if (av_gettime_relative() - t0 > timeout_us) {
            fprintf(stderr, "FAIL: no DVD1 display_buf status at 0x30400008\n");
            return -1;
        }
        av_usleep(500);
    }
}

static int64_t ack_reissue_interval_us(const Player *p)
{
    int64_t T = diag_native_period_us(p);

    if (T <= 0)
        T = ACK_REISSUE_MAX_US;
    if (T < ACK_REISSUE_MIN_US)
        T = ACK_REISSUE_MIN_US;
    if (T > ACK_REISSUE_MAX_US)
        T = ACK_REISSUE_MAX_US;
    return T;
}

static void ack_snapshot(Player *p, int want, uint64_t raw, int magic_ok,
                         int valid_ok, int valid_buf, unsigned reissues,
                         int64_t T_us)
{
    p->last_ack_want = want;
    p->last_ack_raw = raw;
    p->last_ack_magic_ok = magic_ok;
    p->last_ack_valid_ok = valid_ok;
    p->last_ack_valid_buf = valid_buf;
    p->last_ack_reissues = reissues;
    p->last_ack_T_us = T_us;
}

static void log_display_ack_state(const char *title, int want, int64_t waited_us,
                                  uint64_t raw, int valid_ok, int valid_buf,
                                  unsigned reissues, int64_t T_us)
{
    fprintf(stderr,
            "%s:\n"
            "  want=%c\n"
            "  waited_us=%" PRId64 "\n"
            "  last raw mailbox status=0x%016" PRIx64 "\n"
            "  last valid display_buf=%s\n"
            "  request reissues=%u\n"
            "  frame_T_us=%" PRId64 "\n",
            title,
            want ? 'B' : 'A',
            waited_us,
            raw,
            valid_ok ? (valid_buf ? "B" : "A") : "(none)",
            reissues,
            T_us);
}

/* DVD1 status is {magic, display_buf, joystick[30:0]}. An idle pad can
 * rewrite the same bits, so set_seq on DVD2 is the mailbox-FSM heartbeat. */
static void log_ack_watch(const char *why, int want, int display_ok,
                          int display_buf, uint64_t status_word,
                          unsigned status_changes, unsigned joy_changes,
                          unsigned set_seq_changes, int magic_valid,
                          int64_t waited_us)
{
    fprintf(stderr,
            "ACK WATCH: why=%s want=%c display_buf=%s status_word=0x%016"
            PRIx64 " status_changes_since_wait_start=%u joy_changes=%u "
            "set_seq_changes=%u magic_valid=%d waited_us=%" PRId64 "\n",
            why,
            want ? 'B' : 'A',
            display_ok ? (display_buf ? "B" : "A") : "(none)",
            status_word, status_changes, joy_changes, set_seq_changes,
            magic_valid, waited_us);
}

/*
 * Buffer-ownership wait only. timeout_us is a warning threshold (200 ms),
 * not a fatal abort. Reasserts the same mailbox request about once per
 * native frame T. Hard abort at ACK_HARD_TIMEOUT_US.
 */
static int wait_display_buf(Player *p, int want, int64_t timeout_us,
                            int64_t *elapsed_us, int *instant)
{
    int64_t t0 = av_gettime_relative();
    int64_t last_reissue_us = t0;
    int64_t native_T = diag_native_period_us(p);
    int64_t reissue_us = ack_reissue_interval_us(p);
    int64_t warn_us = timeout_us > 0 ? timeout_us : ACK_TIMEOUT_US;
    int first = 1;
    int crossed_warn = 0;
    unsigned reissues = 0;
    int last_valid = 0;
    int last_valid_ok = 0;
    uint64_t last_raw = 0;
    int last_magic_ok = 0;
    uint64_t prev_raw = 0;
    int have_prev_raw = 0;
    unsigned status_changes = 0;
    unsigned joy_changes = 0;
    unsigned set_seq_changes = 0;
    unsigned prev_set_seq = 0;
    int have_prev_seq = 0;

    for (;;) {
        int cur = -1;
        int64_t now = av_gettime_relative();
        int64_t e = now - t0;
        uint64_t setw;
        unsigned seq;

        if (e < 0)
            e = 0;
        last_raw = peek_mbox_status(p->fb);
        last_magic_ok = ((uint32_t)(last_raw >> 32) == JOY_MAGIC);
        if (have_prev_raw) {
            if (last_raw != prev_raw)
                status_changes++;
            if (((uint32_t)last_raw & 0x7fffffffu) !=
                ((uint32_t)prev_raw & 0x7fffffffu))
                joy_changes++;
        }
        prev_raw = last_raw;
        have_prev_raw = 1;
        if (p->fpga_v1_caps) {
            setw = peek_dvd_settings(p->fb);
            if ((uint32_t)(setw >> 32) == SET_MAGIC) {
                seq = (unsigned)((setw >> 16) & 0xffu);
                if (have_prev_seq && seq != prev_set_seq)
                    set_seq_changes++;
                prev_set_seq = seq;
                have_prev_seq = 1;
            }
        }
        if (read_display_buf(p->fb, &cur) == 0) {
            last_valid = cur;
            last_valid_ok = 1;
            if (cur == want) {
                if (elapsed_us)
                    *elapsed_us = e;
                if (instant)
                    *instant = first;
                p->ack_reissue_total += reissues;
                if (reissues)
                    p->ack_wait_reissued++;
                if (native_T > 0) {
                    if (e > native_T)
                        p->ackw_gt1t++;
                    if (e > 2 * native_T)
                        p->ackw_gt2t++;
                    if (e > 3 * native_T)
                        p->ackw_gt3t++;
                }
                if (e >= ACK_TIMEOUT_US)
                    p->ackw_gt200++;
                if (e >= warn_us) {
                    if (!crossed_warn)
                        p->ack_late_n++;
                    p->ack_recovered_late_n++;
                    fprintf(stderr,
                            "DISPLAY ACK recovered after %" PRId64
                            "ms (%u reissues)\n",
                            e / 1000, reissues);
                }
                ack_snapshot(p, want, last_raw, last_magic_ok,
                             last_valid_ok, last_valid, reissues, native_T);
                return 0;
            }
        }
        first = 0;
        if (p->fail || g_interrupt)
            return -1;
        if (!crossed_warn && e >= warn_us) {
            crossed_warn = 1;
            p->ack_late_n++;
            ack_snapshot(p, want, last_raw, last_magic_ok,
                         last_valid_ok, last_valid, reissues, native_T);
            log_display_ack_state("DISPLAY ACK LATE", want, e, last_raw,
                                  last_valid_ok, last_valid, reissues,
                                  native_T);
            log_ack_watch("late", want, last_valid_ok, last_valid, last_raw,
                          status_changes, joy_changes, set_seq_changes,
                          last_magic_ok, e);
        }
        if (e >= ACK_HARD_TIMEOUT_US) {
            ack_snapshot(p, want, last_raw, last_magic_ok,
                         last_valid_ok, last_valid, reissues, native_T);
            fprintf(stderr,
                    "FAIL: display_buf ack timeout (want %c)\n",
                    want ? 'B' : 'A');
            log_display_ack_state("  hard timeout", want, e, last_raw,
                                  last_valid_ok, last_valid, reissues,
                                  native_T);
            fprintf(stderr,
                    "  magic=%s  mbox[0]=%" PRIu32 "\n",
                    last_magic_ok ? "DVD1" : "invalid",
                    (uint32_t)(p->fb->mbox[0] & 1u));
            log_ack_watch("timeout", want, last_valid_ok, last_valid, last_raw,
                          status_changes, joy_changes, set_seq_changes,
                          last_magic_ok, e);
            return -1;
        }
        if (now - last_reissue_us >= reissue_us) {
            /* Idempotent reassertion of the outstanding request. Not a flip. */
            p->fb->mbox[0] = mailbox_ab_word(p->fpga_yuv420, want);
            last_reissue_us = now;
            reissues++;
            if (reissues == 1 || crossed_warn)
                log_ack_watch(reissues == 1 ? "reissue" : "reissue-late",
                              want, last_valid_ok, last_valid, last_raw,
                              status_changes, joy_changes, set_seq_changes,
                              last_magic_ok, e);
            dbg("ack reissue #%u want=%c waited=%.1fms raw=0x%016" PRIx64
                " valid=%s magic=%s\n",
                reissues, want ? 'B' : 'A', e / 1000.0, last_raw,
                last_valid_ok ? (last_valid ? "B" : "A") : "(none)",
                last_magic_ok ? "DVD1" : "invalid");
        }
        av_usleep(500);
    }
}

static void offset_push(int64_t *first, int *first_n, int64_t *last, int *last_n,
                        int64_t *sum, int *n, int64_t *last_off,
                        int64_t *max_pos, int64_t *max_neg, int64_t off)
{
    if (*first_n < OFFSET_LOG_N)
        first[(*first_n)++] = off;
    if (*last_n < OFFSET_LOG_N)
        last[(*last_n)++] = off;
    else {
        memmove(last, last + 1, (OFFSET_LOG_N - 1) * sizeof(last[0]));
        last[OFFSET_LOG_N - 1] = off;
    }
    *sum += off;
    (*n)++;
    *last_off = off;
    if (*n == 1 || off > *max_pos)
        *max_pos = off;
    if (*n == 1 || off < *max_neg)
        *max_neg = off;
}

static void record_offset_pair(Player *p, int64_t raw_off, int64_t adj_off)
{
    offset_push(p->first_raw_offsets, &p->first_raw_off_n,
                p->last_raw_offsets, &p->last_raw_off_n,
                &p->raw_offset_sum, &p->raw_offset_n, &p->last_raw_offset,
                &p->raw_offset_max_pos, &p->raw_offset_max_neg, raw_off);
    offset_push(p->first_offsets, &p->first_off_n,
                p->last_offsets, &p->last_off_n,
                &p->offset_sum, &p->offset_n, &p->last_offset,
                &p->offset_max_pos, &p->offset_max_neg, adj_off);
    if (adj_off < 0)
        p->frames_late++;
    if (adj_off < -40000)
        p->late_40++;
    if (adj_off < -80000)
        p->late_80++;
}

static void record_stale_drop(Player *p, int64_t off)
{
    p->stale_dropped++;
    p->stale_run++;
    if (p->stale_run > p->stale_run_max)
        p->stale_run_max = p->stale_run;
    if (!p->stale_n || off < p->stale_off_min)
        p->stale_off_min = off;
    if (!p->stale_n || off > p->stale_off_max)
        p->stale_off_max = off;
    p->stale_off_sum += off;
    p->stale_n++;
}

static void log_stale_perf(Player *p, int64_t raw_vpts_us, int64_t pvpts_us,
                           int64_t av_delta_us)
{
    int64_t T = frame_duration_us(p);
    int64_t aclk = (pvpts_us != AV_NOPTS_VALUE)
                   ? pvpts_us - av_delta_us : AV_NOPTS_VALUE;
    int64_t since = (p->last_present_end_us > 0)
                    ? av_gettime_relative() - p->last_present_end_us : -1;
    int q = player_buffered(p) ? buffered_queue_count(p) : -1;
    int64_t prev_ack = p->last_path.valid ? p->last_path.ack_wait_us : -1;
    int64_t prev_sws = p->last_path.valid ? p->last_path.sws_wall_us : -1;
    int64_t prev_cyc = p->last_path.valid ? p->last_path.cycle_us : -1;

    if (p->perf_present_no_convert)
        return;
    fprintf(stderr,
            "STALE PERF: raw_vpts=%" PRId64 " pvpts=%" PRId64 " aclk=%" PRId64
            " av_delta_us=%" PRId64 " stale_threshold_us=%" PRId64
            " queue_depth=%d time_since_previous_present_us=%" PRId64
            " previous_ack_wait_us=%" PRId64 " previous_sws_wall_us=%" PRId64
            " previous_cycle_us=%" PRId64 "\n",
            raw_vpts_us, pvpts_us, aclk, av_delta_us,
            T > 0 ? EARLY_SLACK_US - T : 0,
            q, since, prev_ack, prev_sws, prev_cyc);
}

/*
 * True when the following source frame is already due on the MrAudio clock,
 * using presentation_vpts (raw PTS minus N*T). Never drops during preroll
 * (timed==0) or before the consumed clock is ready. Returns 0 if PTS, fps,
 * or the audio clock is not usable.
 */
static int frame_is_stale(Player *p, int64_t vpts_us, int timed, int64_t *off_out)
{
    int64_t aclk, T;
    int ready;

    if (off_out)
        *off_out = 0;
    if (pause_is_held(p))
        return 0;
    if (!timed || vpts_us == AV_NOPTS_VALUE)
        return 0;
    T = frame_duration_us(p);
    if (T <= 0)
        return 0;

    pthread_mutex_lock(&p->clock.mu);
    ready = p->clock.ready;
    aclk = p->clock.clock_us;
    pthread_mutex_unlock(&p->clock.mu);
    if (!ready || aclk == AV_NOPTS_VALUE)
        return 0;

    if (off_out)
        *off_out = vpts_us - aclk;
    return vpts_us + T <= aclk + EARLY_SLACK_US;
}

static const char *fb_letter(int buf)
{
    return buf ? "B" : "A";
}

/*
 * Uncapped throughput path only. Chooses the inactive DDR buffer once and
 * never writes the mailbox, so FPGA scanout keeps displaying the other one.
 */
static int bench_select_inactive_ddr(Player *p)
{
    int on_screen;

    if (wait_valid_display_buf(p, &on_screen, ACK_TIMEOUT_US) < 0)
        return -1;
    p->bench_active_buf = on_screen;
    p->bench_target_buf = on_screen ^ 1;
    p->bench_bufs_ok = 1;
    p->bench_mbox0_start = p->fb->mbox[0];
    p->mailbox_writes = 0;
    dbg("\n=== UNCAPPED VIDEO THROUGHPUT BENCHMARK ===\n"
        "No PTS hold, stale recovery, ACK wait, mailbox, or audio.\n"
        "FPGA display_buf (active/scanout): %s  (bit=%d)\n"
        "Benchmark target (inactive DDR):   %s  phys 0x%08lx\n"
        "Mailbox start word:                0x%08" PRIx32 "\n"
        "sws_scale writes the inactive buffer continuously.\n",
        fb_letter(p->bench_active_buf), p->bench_active_buf,
        fb_letter(p->bench_target_buf),
        p->bench_target_buf ? FB_B_PHYS : FB_A_PHYS,
        p->bench_mbox0_start);
    return 0;
}

static int bench_convert_frame(Player *p, AVFrame *frame, AVCodecContext *vdec,
                               struct SwsContext **sws, int64_t decode_us)
{
    uint8_t *dst_data[4] = {0};
    int dst_linesize[4] = {0};
    int64_t now;
    int64_t cpu0, cpu1, c0, sws_wall, sws_cpu;

    if (player_accept_video_frame(p, frame, vdec) < 0)
        return -1;

    assign_video_pts(p, frame, vdec);
    {
        int64_t vpts_us = (p->assigned_pts != AV_NOPTS_VALUE)
                          ? tb_to_us(p->vtb, p->assigned_pts)
                          : AV_NOPTS_VALUE;
        if (vpts_us != AV_NOPTS_VALUE && p->last_video_pts_us != AV_NOPTS_VALUE) {
            int64_t gap = vpts_us - p->last_video_pts_us;
            if (gap < 0)
                gap = -gap;
            if (gap > DISC_JUMP_US)
                p->video_disc++;
        }
        if (vpts_us != AV_NOPTS_VALUE)
            p->last_video_pts_us = vpts_us;
    }

    p->video_decoded++;
    p->bench_decoded++;
    now = av_gettime_relative();
    if (!p->bench_t0_us)
        p->bench_t0_us = now;

    if (!*sws) {
        AVRational fps0 = detect_fps(p, vdec);
        fprintf(stderr, "Video: %dx%d %s  %.3f fps\n",
                frame->width, frame->height,
                av_get_pix_fmt_name(frame->format)
                    ? av_get_pix_fmt_name(frame->format) : "?",
                (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0);
        dbg("Benchmark video: %dx%d %s  source fps %d/%d (%.3f)\n"
            "Path: decode → sws DIRECT inactive DDR → barrier  "
            "(no ACK, no hold, no mailbox)\n"
            "sws CPU: %s\n",
            frame->width, frame->height,
            av_get_pix_fmt_name(frame->format)
                ? av_get_pix_fmt_name(frame->format) : "?",
            fps0.num, fps0.den,
            (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0,
            p->sws_cpu_ok ? "CLOCK_THREAD_CPUTIME_ID" : "unavailable");
        *sws = sws_getContext(FB_W, player_active_h(p), frame->format,
                              FB_W, player_active_h(p), AV_PIX_FMT_BGR0,
                              SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!*sws) {
            fprintf(stderr, "sws_getContext failed\n");
            player_abort(p);
            return -1;
        }
    }

    dst_data[0] = p->bench_target_buf ? p->fb->fb_b : p->fb->fb_a;
    dst_linesize[0] = FB_STRIDE;
    cpu0 = p->sws_cpu_ok ? thread_cpu_us() : -1;
    c0 = av_gettime_relative();
    sws_scale(*sws, (const uint8_t * const *)frame->data, frame->linesize,
              0, player_active_h(p), dst_data, dst_linesize);
    sws_wall = av_gettime_relative() - c0;
    cpu1 = (p->sws_cpu_ok && cpu0 >= 0) ? thread_cpu_us() : -1;
    __sync_synchronize();
    p->bench_t1_us = av_gettime_relative();
    p->bench_converted++;
    p->convert_us += sws_wall;

    if (p->bench_last_mark_us > 0 && decode_us >= 0)
        stat_add(&p->bench_dec_n, &p->bench_dec_sum, &p->bench_dec_min,
                 &p->bench_dec_max, decode_us);
    stat_add(&p->bench_sws_n, &p->bench_sws_sum, &p->bench_sws_min,
             &p->bench_sws_max, sws_wall);
    sws_cpu = -1;
    if (p->sws_cpu_ok && cpu0 >= 0 && cpu1 >= 0) {
        sws_cpu = cpu1 - cpu0;
        if (sws_cpu < 0)
            sws_cpu = 0;
        stat_add(&p->bench_sws_cpu_n, &p->bench_sws_cpu_sum,
                 &p->bench_sws_cpu_min, &p->bench_sws_cpu_max, sws_cpu);
    }
    if (p->bench_last_mark_us > 0 && decode_us >= 0) {
        int64_t combo = decode_us + sws_wall;
        stat_add(&p->bench_combo_n, &p->bench_combo_sum, &p->bench_combo_min,
                 &p->bench_combo_max, combo);
    }
    p->bench_last_mark_us = p->bench_t1_us;
    return 0;
}

static void present_block_if_held(Player *p)
{
    while (pause_is_held(p) && !p->fail && !g_interrupt) {
        pause_thr_set(&p->pause.st_present, THR_PAUSE);
        pause_wait_unheld(p);
        pause_thr_set(&p->pause.st_present, THR_RUN);
    }
}

static void present_wait_pts(Player *p, int64_t pvpts_us)
{
    int64_t hold0;

    if (pvpts_us == AV_NOPTS_VALUE)
        return;
    hold0 = av_gettime_relative();
    for (;;) {
        int64_t aclk;
        int64_t remain;

        if (pause_is_held(p)) {
            pause_wait_unheld(p);
            hold0 = av_gettime_relative();
            if (p->fail || g_interrupt)
                return;
            continue;
        }
        aclk = clock_read(&p->clock, NULL, NULL);
        if (aclk == AV_NOPTS_VALUE)
            break;
        if (pvpts_us <= aclk + EARLY_SLACK_US)
            break;
        if (av_gettime_relative() - hold0 > MAX_HOLD_US)
            break;
        if (p->fail || g_interrupt)
            break;
        remain = pvpts_us - aclk;
        if (remain > 5000)
            remain = 5000;
        if (remain < 500)
            remain = 500;
        av_usleep((unsigned)remain);
    }
}

static int present_video_frame(Player *p, AVFrame *frame, AVCodecContext *vdec,
                               struct SwsContext **sws, int timed)
{
    uint8_t *dst_data[4] = {0};
    int dst_linesize[4] = {0};

    if (player_accept_video_frame(p, frame, vdec) < 0)
        return -1;

    assign_video_pts(p, frame, vdec);
    int64_t vpts_us = (p->assigned_pts != AV_NOPTS_VALUE)
                      ? tb_to_us(p->vtb, p->assigned_pts)
                      : AV_NOPTS_VALUE;
    if (vpts_us != AV_NOPTS_VALUE && p->last_video_pts_us != AV_NOPTS_VALUE) {
        int64_t gap = vpts_us - p->last_video_pts_us;
        if (gap < 0)
            gap = -gap;
        if (gap > DISC_JUMP_US)
            p->video_disc++;
    }
    if (vpts_us != AV_NOPTS_VALUE)
        p->last_video_pts_us = vpts_us;

    int64_t pvpts_us = presentation_vpts_from_raw(p, vpts_us);

    present_block_if_held(p);
    if (p->fail || g_interrupt)
        return -1;

    {
        int64_t stale_off = 0;
        if (frame_is_stale(p, pvpts_us, timed, &stale_off)) {
            log_stale_perf(p, vpts_us, pvpts_us, stale_off);
            record_stale_drop(p, stale_off);
            return 1;
        }
    }

    if (!*sws) {
        AVRational fps0 = detect_fps(p, vdec);
        int64_t T0 = frame_duration_us(p);
        fprintf(stderr, "Video: %dx%d %s  %.3f fps\n",
                frame->width, frame->height,
                av_get_pix_fmt_name(frame->format)
                    ? av_get_pix_fmt_name(frame->format) : "?",
                (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0);
        dbg("\n=== VIDEO PATH (threaded, audio-master) ===\n"
            "%dx%d %s  fps %d/%d (%.3f)  present=FPGA display_buf ack\n"
            "Path: decode → stale-check → ACK wait → sws DIRECT DDR → "
            "barrier → PTS +2ms → mailbox\n"
            "Frame duration T=%.3f ms  stale if pres v-a <= %+.3f ms\n"
            "Presentation phase: %d frame%s / %.3f ms  "
            "(hold and stale use raw_vpts - N*T)\n"
            "Path diag: decode=mailbox-return→receive_frame; "
            "cycle=ack-wait→mailbox; "
            "mbox-cycle=previous-mailbox→this-mailbox; sws CPU=%s\n",
            frame->width, frame->height,
            av_get_pix_fmt_name(frame->format)
                ? av_get_pix_fmt_name(frame->format) : "?",
            fps0.num, fps0.den,
            (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0,
            T0 / 1000.0,
            T0 > 0 ? (EARLY_SLACK_US - T0) / 1000.0 : 0.0,
            p->initial_skip_req, p->initial_skip_req == 1 ? "" : "s",
            presentation_phase_us(p) / 1000.0,
            p->sws_cpu_ok ? "CLOCK_THREAD_CPUTIME_ID" : "unavailable");
        if (!p->fpga_yuv420) {
            *sws = sws_getContext(FB_W, player_active_h(p), frame->format,
                                  FB_W, player_active_h(p), AV_PIX_FMT_BGR0,
                                  SWS_FAST_BILINEAR, NULL, NULL, NULL);
            if (!*sws) {
                fprintf(stderr, "sws_getContext failed\n");
                player_abort(p);
                return -1;
            }
        }
    }

    /* Previous mailbox dest is in flight until FPGA display_buf matches it.
     * That ack frees the other DDR buffer. Skip on the first present. */
    int64_t prev_mbox_us = p->last_present_end_us;
    int64_t cycle_t0 = av_gettime_relative();
    int64_t ack_wait_us = 0;
    int ack_instant = 0;
    int ack_waited = 0;
    if (p->flips > 0) {
        if (wait_display_buf(p, p->displayed, ACK_TIMEOUT_US,
                             &ack_wait_us, &ack_instant) < 0) {
            player_abort(p);
            return -1;
        }
        ack_waited = 1;
        note_display_ack(p, av_gettime_relative());
    }

    int on_screen;
    if (wait_valid_display_buf(p, &on_screen, ACK_TIMEOUT_US) < 0) {
        player_abort(p);
        return -1;
    }

    int next = on_screen ^ 1;
    if (next == on_screen) {
        fprintf(stderr, "FAIL: dest buffer is display_buf\n");
        player_abort(p);
        return -1;
    }
    if (p->flips > 0 && next == p->displayed) {
        fprintf(stderr, "FAIL: dest is the unacknowledged mailbox request\n");
        player_abort(p);
        return -1;
    }

    int64_t own0 = av_gettime_relative();
    dst_data[0] = next ? p->fb->fb_b : p->fb->fb_a;
    dst_linesize[0] = FB_STRIDE;
    int64_t cpu0 = p->sws_cpu_ok ? thread_cpu_us() : -1;
    int64_t c0 = av_gettime_relative();
    int64_t sws_wall;
    int64_t cpu1;
    if (p->fpga_yuv420) {
        log_yuv_frame_meta_once(p, frame);
        if (copy_yuv420_to_slot(dst_data[0], frame, player_active_h(p)) < 0) {
            fprintf(stderr, "FAIL: FPGA YUV420 plane copy\n");
            player_abort(p);
            return -1;
        }
        sws_wall = av_gettime_relative() - c0;
        cpu1 = (p->sws_cpu_ok && cpu0 >= 0) ? thread_cpu_us() : -1;
        p->convert_us += sws_wall;
        yuv_copy_note(p, sws_wall);
    } else {
        sws_scale(*sws, (const uint8_t * const *)frame->data, frame->linesize,
                  0, player_active_h(p), dst_data, dst_linesize);
        sws_wall = av_gettime_relative() - c0;
        cpu1 = (p->sws_cpu_ok && cpu0 >= 0) ? thread_cpu_us() : -1;
        p->convert_us += sws_wall;
        if (p->in_menu || g_debug_yellow_highlight)
            present_draw_highlight(p, dst_data[0], dst_linesize[0], p->in_menu);
        else
            movie_sub_overlay(p, dst_data[0], dst_linesize[0], vpts_us, NULL);
    }
    __sync_synchronize();
    int64_t conv_done = av_gettime_relative();

    if (timed && pvpts_us != AV_NOPTS_VALUE)
        present_wait_pts(p, pvpts_us);

    int64_t aclk = clock_read(&p->clock, NULL, NULL);
    int64_t av_off = 0;
    if (timed && aclk != AV_NOPTS_VALUE && vpts_us != AV_NOPTS_VALUE) {
        int64_t raw_off = vpts_us - aclk;
        av_off = (pvpts_us != AV_NOPTS_VALUE) ? pvpts_us - aclk : raw_off;
        record_offset_pair(p, raw_off, av_off);
    }

    p->fb->mbox[0] = mailbox_ab_word(p->fpga_yuv420, next);
    p->last_mbox_wall_us = av_gettime_relative();
    if (next)
        p->frames_b++;
    else
        p->frames_a++;
    p->flips++;
    p->displayed = next;
    p->rendered++;
    p->stale_run = 0;
    p->last_present_end_us = p->last_mbox_wall_us;
    pause_note_resume_video_pts(p, vpts_us);

    /* Path stats after mailbox so they cannot delay the FPGA request. */
    {
        int64_t a2m = p->last_mbox_wall_us - own0;
        int64_t c2m = p->last_mbox_wall_us - conv_done;
        int64_t cycle = p->last_mbox_wall_us - cycle_t0;
        int64_t sws_cpu = -1, preempt = -1;

        if (a2m < 0)
            a2m = 0;
        if (c2m < 0)
            c2m = 0;
        if (cycle < 0)
            cycle = 0;

        if (ack_waited) {
            stat_add(&p->ackw_n, &p->ackw_sum, &p->ackw_min, &p->ackw_max,
                     ack_wait_us);
            if (ack_instant)
                p->ackw_instant++;
            if (ack_wait_us > 5000)
                p->ackw_gt5++;
            if (ack_wait_us > 10000)
                p->ackw_gt10++;
            if (ack_wait_us > 20000)
                p->ackw_gt20++;
            if (ack_wait_us > 30000)
                p->ackw_gt30++;
            if (ack_wait_us > 40000)
                p->ackw_gt40++;
        }

        stat_add(&p->sws_n, &p->sws_sum, &p->sws_min, &p->sws_max, sws_wall);
        if (sws_wall > 20000)
            p->sws_gt20++;
        if (sws_wall > 25000)
            p->sws_gt25++;
        if (sws_wall > 30000)
            p->sws_gt30++;
        if (sws_wall > 35000)
            p->sws_gt35++;
        if (sws_wall > 40000)
            p->sws_gt40++;
        if (sws_wall > 50000)
            p->sws_gt50++;
        if (p->sws_cpu_ok && cpu0 >= 0 && cpu1 >= 0) {
            sws_cpu = cpu1 - cpu0;
            if (sws_cpu < 0)
                sws_cpu = 0;
            preempt = sws_wall - sws_cpu;
            if (preempt < 0)
                preempt = 0;
            stat_add(&p->sws_cpu_n, &p->sws_cpu_sum, &p->sws_cpu_min,
                     &p->sws_cpu_max, sws_cpu);
            if (p->sws_cpu_n == 1) {
                p->preempt_min = preempt;
                p->preempt_max = preempt;
                p->preempt_sum = preempt;
            } else {
                p->preempt_sum += preempt;
                if (preempt < p->preempt_min)
                    p->preempt_min = preempt;
                if (preempt > p->preempt_max)
                    p->preempt_max = preempt;
            }
            if (preempt > 1000)
                p->preempt_gt1++;
            if (preempt > 2000)
                p->preempt_gt2++;
            if (preempt > 5000)
                p->preempt_gt5++;
            if (preempt > 10000)
                p->preempt_gt10++;
        }

        if (prev_mbox_us > 0) {
            int64_t mc = p->last_mbox_wall_us - prev_mbox_us;
            stat_add(&p->mbox_cyc_n, &p->mbox_cyc_sum, &p->mbox_cyc_min,
                     &p->mbox_cyc_max, mc);
        }

        stat_add(&p->a2m_n, &p->a2m_sum, &p->a2m_min, &p->a2m_max, a2m);
        if (a2m > 20000)
            p->a2m_gt20++;
        if (a2m > 30000)
            p->a2m_gt30++;
        if (a2m > 35000)
            p->a2m_gt35++;
        if (a2m > 40000)
            p->a2m_gt40++;
        if (a2m > 50000)
            p->a2m_gt50++;
        stat_add(&p->c2m_n, &p->c2m_sum, &p->c2m_min, &p->c2m_max, c2m);
        stat_add(&p->cyc_n, &p->cyc_sum, &p->cyc_min, &p->cyc_max, cycle);
        if (cycle > 30000)
            p->cyc_gt30++;
        if (cycle > 35000)
            p->cyc_gt35++;
        if (cycle > 40000)
            p->cyc_gt40++;
        if (cycle > 50000)
            p->cyc_gt50++;
        if (cycle > 80000)
            p->cyc_gt80++;

        p->last_path.valid = 1;
        p->last_path.frame = p->rendered;
        p->last_path.vpts_us = vpts_us;
        p->last_path.av_off_us = av_off;
        p->last_path.ack_wait_us = ack_wait_us;
        p->last_path.ack_instant = ack_instant;
        p->last_path.ack_waited = ack_waited;
        p->last_path.decode_us = p->cur_decode_us;
        p->last_path.sws_wall_us = sws_wall;
        p->last_path.sws_cpu_us = sws_cpu;
        p->last_path.preempt_us = preempt;
        p->last_path.ack_to_mbox_us = a2m;
        p->last_path.conv_to_mbox_us = c2m;
        p->last_path.cycle_us = cycle;
    }
    return 0;
}

static int producer_enqueue_frame(Player *p, AVFrame *frame, AVCodecContext *vdec,
                                  struct SwsContext **sws, int64_t decode_us)
{
    uint8_t *dst_data[4] = {0};
    int dst_linesize[4] = {0};
    int idx;
    int64_t cpu0, cpu1, c0, sws_wall, sws_cpu;

    if (player_accept_video_frame(p, frame, vdec) < 0)
        return -1;

    assign_video_pts(p, frame, vdec);
    {
        int64_t vpts_us = (p->assigned_pts != AV_NOPTS_VALUE)
                          ? tb_to_us(p->vtb, p->assigned_pts)
                          : AV_NOPTS_VALUE;
        if (vpts_us != AV_NOPTS_VALUE && p->last_video_pts_us != AV_NOPTS_VALUE) {
            int64_t gap = vpts_us - p->last_video_pts_us;
            if (gap < 0)
                gap = -gap;
            if (gap > DISC_JUMP_US)
                p->video_disc++;
        }
        if (vpts_us != AV_NOPTS_VALUE)
            p->last_video_pts_us = vpts_us;
    }

    if (!*sws) {
        AVRational fps0 = detect_fps(p, vdec);
        fprintf(stderr, "Video: %dx%d %s  %.3f fps\n",
                frame->width, frame->height,
                av_get_pix_fmt_name(frame->format)
                    ? av_get_pix_fmt_name(frame->format) : "?",
                (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0);
        dbg("\n=== VIDEO PRODUCER (cached RAM ring) ===\n"
            "%dx%d %s  fps %d/%d (%.3f)\n"
            "Path: decode → sws into cached BGR0 slot → enqueue\n"
            "No ACK, DDR, PTS hold, mailbox, or stale recovery here.\n"
            "Ring %d frames, prefill %d. sws CPU: %s\n",
            frame->width, frame->height,
            av_get_pix_fmt_name(frame->format)
                ? av_get_pix_fmt_name(frame->format) : "?",
            fps0.num, fps0.den,
            (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0,
            VIDEO_BUFFER_FRAMES, VIDEO_PREFILL_FRAMES,
            p->sws_cpu_ok ? "CLOCK_THREAD_CPUTIME_ID" : "unavailable");
        *sws = sws_getContext(FB_W, player_active_h(p), frame->format,
                              FB_W, player_active_h(p), AV_PIX_FMT_BGR0,
                              SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!*sws) {
            fprintf(stderr, "sws_getContext failed\n");
            player_abort(p);
            return -1;
        }
    }

    pause_thr_set(&p->pause.st_video, THR_QFULL);
    if (!vidring_begin_produce(&p->vring, &idx, p)) {
        pause_thr_set(&p->pause.st_video, THR_RUN);
        return p->fail ? -1 : 1;
    }
    pause_thr_set(&p->pause.st_video, THR_RUN);
    if (p->fail)
        return -1;

    dst_data[0] = p->vring.slots[idx].bgr0;
    dst_linesize[0] = FB_STRIDE;
    cpu0 = p->sws_cpu_ok ? thread_cpu_us() : -1;
    c0 = av_gettime_relative();
    sws_scale(*sws, (const uint8_t * const *)frame->data, frame->linesize,
              0, player_active_h(p), dst_data, dst_linesize);
    sws_wall = av_gettime_relative() - c0;
    cpu1 = (p->sws_cpu_ok && cpu0 >= 0) ? thread_cpu_us() : -1;

    p->video_decoded++;
    p->prod_enqueued++;
    if (!p->prod_t0_us)
        p->prod_t0_us = c0;
    p->prod_t1_us = av_gettime_relative();

    if (p->prod_last_end_us > 0 && decode_us >= 0)
        stat_add(&p->prod_dec_n, &p->prod_dec_sum, &p->prod_dec_min,
                 &p->prod_dec_max, decode_us);
    stat_add(&p->prod_sws_n, &p->prod_sws_sum, &p->prod_sws_min,
             &p->prod_sws_max, sws_wall);
    if (p->sws_cpu_ok && cpu0 >= 0 && cpu1 >= 0) {
        sws_cpu = cpu1 - cpu0;
        if (sws_cpu < 0)
            sws_cpu = 0;
        stat_add(&p->prod_sws_cpu_n, &p->prod_sws_cpu_sum,
                 &p->prod_sws_cpu_min, &p->prod_sws_cpu_max, sws_cpu);
    }

    vidring_commit_produce(&p->vring, idx,
                           (p->assigned_pts != AV_NOPTS_VALUE)
                               ? tb_to_us(p->vtb, p->assigned_pts)
                               : AV_NOPTS_VALUE,
                           p->buf_playing);
    p->prod_last_end_us = av_gettime_relative();

    if (!prefill_is_released(p) &&
        vidring_count(&p->vring) >= p->prefill_req)
        prefill_release(p, "prefill target reached");
    return 0;
}

static int producer_enqueue_yuv(Player *p, AVFrame *frame, AVCodecContext *vdec,
                                int64_t decode_us, unsigned nav_gen)
{
    int idx;
    int64_t now;
    unsigned epoch;

    if (player_accept_video_frame(p, frame, vdec) < 0)
        return -1;

    assign_video_pts(p, frame, vdec);
    {
        int64_t vpts_us = (p->assigned_pts != AV_NOPTS_VALUE)
                          ? tb_to_us(p->vtb, p->assigned_pts)
                          : AV_NOPTS_VALUE;
        if (vpts_us != AV_NOPTS_VALUE && p->last_video_pts_us != AV_NOPTS_VALUE) {
            int64_t gap = vpts_us - p->last_video_pts_us;
            if (gap < 0)
                gap = -gap;
            if (gap > DISC_JUMP_US)
                p->video_disc++;
        }
        if (vpts_us != AV_NOPTS_VALUE)
            p->last_video_pts_us = vpts_us;
    }

    if (!p->prod_t0_us) {
        AVRational fps0 = detect_fps(p, vdec);
        fprintf(stderr, "Video: %dx%d %s  %.3f fps\n",
                frame->width, frame->height,
                av_get_pix_fmt_name(frame->format)
                    ? av_get_pix_fmt_name(frame->format) : "?",
                (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0);
        dbg("\n=== VIDEO PRODUCER (decoded YUV queue) ===\n"
            "%dx%d %s  fps %d/%d (%.3f)\n"
            "Path: decode → av_frame_ref into queue slot\n"
            "No sws, ACK, DDR, PTS hold, mailbox, or stale recovery here.\n"
            "Queue %d frames, prefill %d. AVFrame refs, no YUV plane copies.\n",
            frame->width, frame->height,
            av_get_pix_fmt_name(frame->format)
                ? av_get_pix_fmt_name(frame->format) : "?",
            fps0.num, fps0.den,
            (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0,
            VIDEO_BUFFER_FRAMES, VIDEO_PREFILL_FRAMES);
    }

    pause_thr_set(&p->pause.st_video, THR_QFULL);
    if (!yuvring_begin_produce(&p->yuvring, &idx, &epoch, p)) {
        pause_thr_set(&p->pause.st_video, THR_RUN);
        return p->fail ? -1 : 1;
    }
    pause_thr_set(&p->pause.st_video, THR_RUN);
    if (p->fail)
        return -1;

    av_frame_unref(p->yuvring.slots[idx].yuv);
    if (av_frame_ref(p->yuvring.slots[idx].yuv, frame) < 0) {
        fprintf(stderr, "av_frame_ref failed for YUV queue slot\n");
        player_abort(p);
        return -1;
    }
    p->yuvring.slots[idx].width = frame->width;
    p->yuvring.slots[idx].height = frame->height;
    p->yuvring.slots[idx].format = frame->format;

    now = av_gettime_relative();
    p->video_decoded++;
    p->prod_enqueued++;
    if (!p->prod_t0_us)
        p->prod_t0_us = now;
    p->prod_t1_us = now;

    if (p->prod_last_end_us > 0 && decode_us >= 0)
        stat_add(&p->prod_dec_n, &p->prod_dec_sum, &p->prod_dec_min,
                 &p->prod_dec_max, decode_us);

    yuvring_commit_produce(&p->yuvring, idx,
                           (p->assigned_pts != AV_NOPTS_VALUE)
                               ? tb_to_us(p->vtb, p->assigned_pts)
                               : AV_NOPTS_VALUE,
                           p->buf_playing, epoch, nav_gen, p->in_menu);
    p->prod_last_end_us = av_gettime_relative();

    if (p->soft_log_yuv && nav_gen == p->soft_nav_gen) {
        p->soft_log_yuv = 0;
        dbg("DVD MENU: first YUV enqueue after SOFT HOP  gen=%u  "
            "queue=%d\n",
            nav_gen, yuvring_count(&p->yuvring));
    }

    if (!prefill_is_released(p)) {
        int n = yuvring_count(&p->yuvring);
        if (n >= p->prefill_req)
            prefill_release(p, "prefill target reached");
        else if (p->in_menu && n >= 1)
            prefill_release(p, "menu/still picture");
    }
    return 0;
}

static int present_cached_frame(Player *p, uint8_t *src, int64_t vpts_us)
{
    int64_t pvpts_us = presentation_vpts_from_raw(p, vpts_us);
    int64_t stale_off = 0;

    present_block_if_held(p);
    if (p->fail || g_interrupt)
        return -1;

    if (frame_is_stale(p, pvpts_us, 1, &stale_off)) {
        log_stale_perf(p, vpts_us, pvpts_us, stale_off);
        record_stale_drop(p, stale_off);
        return 1;
    }

    if (p->rendered == 0) {
        int64_t T0 = frame_duration_us(p);
        dbg("\n=== VIDEO CONSUMER (cached RAM → DDR) ===\n"
            "Path: queue pop → stale-check → ACK wait → memcpy RAM→DDR → "
            "barrier → PTS +2ms → mailbox\n"
            "Video pipeline: BUFFERED CACHED-RAM producer/consumer\n"
            "Frame duration T=%.3f ms  stale if pres v-a <= %+.3f ms\n"
            "Presentation phase: %d frame%s / %.3f ms\n",
            T0 / 1000.0,
            T0 > 0 ? (EARLY_SLACK_US - T0) / 1000.0 : 0.0,
            p->initial_skip_req, p->initial_skip_req == 1 ? "" : "s",
            presentation_phase_us(p) / 1000.0);
    }

    int64_t prev_mbox_us = p->last_present_end_us;
    int64_t cycle_t0 = av_gettime_relative();
    int64_t ack_wait_us = 0;
    int ack_instant = 0;
    int ack_waited = 0;
    if (p->flips > 0) {
        if (wait_display_buf(p, p->displayed, ACK_TIMEOUT_US,
                             &ack_wait_us, &ack_instant) < 0) {
            player_abort(p);
            return -1;
        }
        ack_waited = 1;
        note_display_ack(p, av_gettime_relative());
    }

    int on_screen;
    if (wait_valid_display_buf(p, &on_screen, ACK_TIMEOUT_US) < 0) {
        player_abort(p);
        return -1;
    }

    int next = on_screen ^ 1;
    if (next == on_screen) {
        fprintf(stderr, "FAIL: dest buffer is display_buf\n");
        player_abort(p);
        return -1;
    }
    if (p->flips > 0 && next == p->displayed) {
        fprintf(stderr, "FAIL: dest is the unacknowledged mailbox request\n");
        player_abort(p);
        return -1;
    }

    int64_t own0 = av_gettime_relative();
    uint8_t *dst = next ? p->fb->fb_b : p->fb->fb_a;
    int64_t cpu0 = p->sws_cpu_ok ? thread_cpu_us() : -1;
    int64_t c0 = av_gettime_relative();
    memcpy(dst, src, player_active_fb_bytes(p));
    int64_t copy_wall = av_gettime_relative() - c0;
    int64_t cpu1 = (p->sws_cpu_ok && cpu0 >= 0) ? thread_cpu_us() : -1;
    p->convert_us += copy_wall;
    __sync_synchronize();
    int64_t conv_done = av_gettime_relative();

    if (pvpts_us != AV_NOPTS_VALUE)
        present_wait_pts(p, pvpts_us);

    int64_t aclk = clock_read(&p->clock, NULL, NULL);
    int64_t av_off = 0;
    if (aclk != AV_NOPTS_VALUE && vpts_us != AV_NOPTS_VALUE) {
        int64_t raw_off = vpts_us - aclk;
        av_off = (pvpts_us != AV_NOPTS_VALUE) ? pvpts_us - aclk : raw_off;
        record_offset_pair(p, raw_off, av_off);
    }

    p->fb->mbox[0] = mailbox_ab_word(p->fpga_yuv420, next);
    p->last_mbox_wall_us = av_gettime_relative();
    if (next)
        p->frames_b++;
    else
        p->frames_a++;
    p->flips++;
    p->displayed = next;
    p->rendered++;
    p->stale_run = 0;
    p->last_present_end_us = p->last_mbox_wall_us;
    pause_note_resume_video_pts(p, vpts_us);

    {
        int64_t a2m = p->last_mbox_wall_us - own0;
        int64_t c2m = p->last_mbox_wall_us - conv_done;
        int64_t cycle = p->last_mbox_wall_us - cycle_t0;
        int64_t copy_cpu = -1, preempt = -1;

        if (a2m < 0)
            a2m = 0;
        if (c2m < 0)
            c2m = 0;
        if (cycle < 0)
            cycle = 0;

        if (ack_waited) {
            stat_add(&p->ackw_n, &p->ackw_sum, &p->ackw_min, &p->ackw_max,
                     ack_wait_us);
            if (ack_instant)
                p->ackw_instant++;
            if (ack_wait_us > 5000)
                p->ackw_gt5++;
            if (ack_wait_us > 10000)
                p->ackw_gt10++;
            if (ack_wait_us > 20000)
                p->ackw_gt20++;
            if (ack_wait_us > 30000)
                p->ackw_gt30++;
            if (ack_wait_us > 40000)
                p->ackw_gt40++;
        }

        stat_add(&p->memcpy_n, &p->memcpy_sum, &p->memcpy_min, &p->memcpy_max,
                 copy_wall);
        if (copy_wall > 20000)
            p->memcpy_gt20++;
        if (copy_wall > 25000)
            p->memcpy_gt25++;
        if (copy_wall > 30000)
            p->memcpy_gt30++;
        if (copy_wall > 35000)
            p->memcpy_gt35++;
        if (copy_wall > 40000)
            p->memcpy_gt40++;
        if (copy_wall > 50000)
            p->memcpy_gt50++;
        if (p->sws_cpu_ok && cpu0 >= 0 && cpu1 >= 0) {
            copy_cpu = cpu1 - cpu0;
            if (copy_cpu < 0)
                copy_cpu = 0;
            preempt = copy_wall - copy_cpu;
            if (preempt < 0)
                preempt = 0;
            stat_add(&p->memcpy_cpu_n, &p->memcpy_cpu_sum, &p->memcpy_cpu_min,
                     &p->memcpy_cpu_max, copy_cpu);
            if (p->memcpy_cpu_n == 1) {
                p->preempt_min = preempt;
                p->preempt_max = preempt;
                p->preempt_sum = preempt;
            } else {
                p->preempt_sum += preempt;
                if (preempt < p->preempt_min)
                    p->preempt_min = preempt;
                if (preempt > p->preempt_max)
                    p->preempt_max = preempt;
            }
            if (preempt > 1000)
                p->preempt_gt1++;
            if (preempt > 2000)
                p->preempt_gt2++;
            if (preempt > 5000)
                p->preempt_gt5++;
            if (preempt > 10000)
                p->preempt_gt10++;
        }

        if (prev_mbox_us > 0) {
            int64_t mc = p->last_mbox_wall_us - prev_mbox_us;
            stat_add(&p->mbox_cyc_n, &p->mbox_cyc_sum, &p->mbox_cyc_min,
                     &p->mbox_cyc_max, mc);
        }

        stat_add(&p->a2m_n, &p->a2m_sum, &p->a2m_min, &p->a2m_max, a2m);
        if (a2m > 20000)
            p->a2m_gt20++;
        if (a2m > 30000)
            p->a2m_gt30++;
        if (a2m > 35000)
            p->a2m_gt35++;
        if (a2m > 40000)
            p->a2m_gt40++;
        if (a2m > 50000)
            p->a2m_gt50++;
        stat_add(&p->c2m_n, &p->c2m_sum, &p->c2m_min, &p->c2m_max, c2m);
        stat_add(&p->cyc_n, &p->cyc_sum, &p->cyc_min, &p->cyc_max, cycle);
        if (cycle > 30000)
            p->cyc_gt30++;
        if (cycle > 35000)
            p->cyc_gt35++;
        if (cycle > 40000)
            p->cyc_gt40++;
        if (cycle > 50000)
            p->cyc_gt50++;
        if (cycle > 80000)
            p->cyc_gt80++;

        p->last_path.valid = 1;
        p->last_path.frame = p->rendered;
        p->last_path.vpts_us = vpts_us;
        p->last_path.av_off_us = av_off;
        p->last_path.ack_wait_us = ack_wait_us;
        p->last_path.ack_instant = ack_instant;
        p->last_path.ack_waited = ack_waited;
        p->last_path.decode_us = p->cur_decode_us;
        p->last_path.sws_wall_us = copy_wall;
        p->last_path.sws_cpu_us = copy_cpu;
        p->last_path.preempt_us = preempt;
        p->last_path.ack_to_mbox_us = a2m;
        p->last_path.conv_to_mbox_us = c2m;
        p->last_path.cycle_us = cycle;
    }
    return 0;
}

static void draw_highlight_border(uint8_t *bgr0, int stride,
                                  int sx, int sy, int ex, int ey, int max_h)
{
    int x, y, t;
    uint32_t pix = 0x00FFFF00u; /* BGR0 LE: B=0 G=255 R=255 */

    if (!bgr0 || ex <= sx || ey <= sy)
        return;
    if (sx < 0)
        sx = 0;
    if (sy < 0)
        sy = 0;
    if (ex >= FB_W)
        ex = FB_W - 1;
    if (max_h < 1)
        max_h = FB_H;
    if (ey >= max_h)
        ey = max_h - 1;
    if (ex <= sx || ey <= sy)
        return;
    for (t = 0; t < HL_BORDER_PX; t++) {
        int y0 = sy + t, y1 = ey - t;
        int x0 = sx + t, x1 = ex - t;
        if (y0 > y1 || x0 > x1)
            break;
        for (x = x0; x <= x1; x++) {
            *(uint32_t *)(bgr0 + (size_t)y0 * (size_t)stride + (size_t)x * 4) = pix;
            *(uint32_t *)(bgr0 + (size_t)y1 * (size_t)stride + (size_t)x * 4) = pix;
        }
        for (y = y0; y <= y1; y++) {
            *(uint32_t *)(bgr0 + (size_t)y * (size_t)stride + (size_t)x0 * 4) = pix;
            *(uint32_t *)(bgr0 + (size_t)y * (size_t)stride + (size_t)x1 * 4) = pix;
        }
    }
}

static void present_draw_highlight(Player *p, uint8_t *dst, int stride,
                                   int frame_menu)
{
    int vis, sx, sy, ex, ey, in_menu;
    unsigned gen;

    if (!p->hl.inited)
        return;
    pthread_mutex_lock(&p->hl.mu);
    vis = p->hl.visible;
    in_menu = p->hl.in_menu;
    gen = p->hl.gen;
    sx = p->hl.sx;
    sy = p->hl.sy;
    ex = p->hl.ex;
    ey = p->hl.ey;
    pthread_mutex_unlock(&p->hl.mu);
    if (g_debug_yellow_highlight) {
        if (vis && in_menu && frame_menu && gen == player_nav_gen(p))
            draw_highlight_border(dst, stride, sx, sy, ex, ey,
                                  player_active_h(p));
        return;
    }
    menu_overlay_composite(p, dst, stride, frame_menu);
}

static int present_menu_skip_clock(Player *p, int ui_redraw, int frame_menu)
{
    if (ui_redraw)
        return 1;
    if (frame_menu || p->in_menu || p->still_active)
        return 1;
    return 0;
}

/*
 * phase_us is mailbox-write -> display_buf ACK of the previous request
 * (p->last_mbox_to_ack_us). It is NOT a raster/latch phase: ARM cannot
 * observe the FPGA line counters without inventing a clock.
 */
static void log_present_perf(Player *p, int64_t raw_vpts_us, int64_t pvpts_us,
                             int64_t aclk, int64_t av_delta_us,
                             int64_t ack_wait_us, int ack_instant,
                             int64_t sws_wall_us, int64_t post_sws_wait_us,
                             int64_t cycle_us, int64_t mbox_to_ack_us,
                             int64_t mbox_write_us, int display_before,
                             int dest, int display_after, uint64_t status_word)
{
    int q = player_buffered(p) ? buffered_queue_count(p) : -1;

    fprintf(stderr,
            "PRESENT PERF: raw_vpts=%" PRId64 " pvpts=%" PRId64 " aclk=%" PRId64
            " av_delta_us=%" PRId64 " ack_wait_us=%" PRId64 " ack_instant=%d"
            " %s=%" PRId64 " post_sws_wait_us=%" PRId64
            " cycle_us=%" PRId64 " mbox_to_ack_us=%" PRId64
            " mbox_write_us=%" PRId64 " queue_depth=%d osd_trim_ms=%d"
            " video_advance_ms=%d initial_skip=%d display_before=%c dest=%c"
            " display_after=%c phase_us=%" PRId64 " status_word=0x%016" PRIx64
            "\n",
            raw_vpts_us, pvpts_us, aclk, av_delta_us, ack_wait_us, ack_instant,
            p->fpga_yuv420 ? "yuv_copy_us" : "sws_wall_us",
            sws_wall_us, post_sws_wait_us, cycle_us, mbox_to_ack_us,
            mbox_write_us, q, p->osd_av_trim_ms, p->video_advance_ms,
            p->initial_skip_req,
            display_before ? 'B' : 'A', dest ? 'B' : 'A',
            display_after ? 'B' : 'A',
            mbox_to_ack_us, status_word);
}

static int present_yuv_frame(Player *p, AVFrame *frame, int64_t vpts_us,
                             struct SwsContext **sws, int ui_redraw,
                             int frame_menu)
{
    uint8_t *dst_data[4] = {0};
    int dst_linesize[4] = {0};
    int64_t pvpts_us = presentation_vpts_from_raw(p, vpts_us);
    int64_t stale_off = 0;
    int skip_clock = present_menu_skip_clock(p, ui_redraw, frame_menu);
    int skip_sws = p->perf_present_no_convert &&
                   (p->iso_warm_presents >= ISO_WARM_PRESENTS);

    if (player_accept_video_frame(p, frame, NULL) < 0)
        return -1;

    if (!skip_clock) {
        present_block_if_held(p);
        if (p->fail || g_interrupt)
            return -1;
        if (frame_is_stale(p, pvpts_us, 1, &stale_off)) {
            if (movie_sub_would_be_visible(p, vpts_us))
                p->subperf.act_stale++;
            else
                p->subperf.inact_stale++;
            log_stale_perf(p, vpts_us, pvpts_us, stale_off);
            record_stale_drop(p, stale_off);
            return 1;
        }
    }

    int64_t decision_aclk = skip_clock ? AV_NOPTS_VALUE
                                       : clock_read(&p->clock, NULL, NULL);
    int64_t decision_delta = 0;
    if (!skip_clock && pvpts_us != AV_NOPTS_VALUE &&
        decision_aclk != AV_NOPTS_VALUE)
        decision_delta = pvpts_us - decision_aclk;

    if (p->rendered == 0) {
        int64_t T0 = frame_duration_us(p);
        dbg("\n=== VIDEO CONSUMER (YUV queue → direct DDR sws) ===\n"
            "Path: queue pop → stale-check → ACK wait → "
            "%s → barrier → PTS +2ms → mailbox\n"
            "Video pipeline: BUFFERED YUV producer / direct-DDR sws "
            "consumer\n"
            "Frame duration T=%.3f ms  stale if pres v-a <= %+.3f ms\n"
            "Initial frame advance: %d frame%s / %.3f ms\n"
            "Additional video advance: %.3f ms\n"
            "Total presentation phase: %.3f ms  (hold and stale)\n"
            "sws CPU: %s\n",
            p->fpga_yuv420
                ? "copy YUV planes to DDR"
                : p->perf_present_no_convert
                ? "warmup sws then skip convert"
                : "sws YUV DIRECT DDR",
            T0 / 1000.0,
            T0 > 0 ? (EARLY_SLACK_US - T0) / 1000.0 : 0.0,
            p->initial_skip_req, p->initial_skip_req == 1 ? "" : "s",
            presentation_phase_us(p) / 1000.0,
            video_advance_applied_us(p) / 1000.0,
            total_presentation_phase_us(p) / 1000.0,
            p->sws_cpu_ok ? "CLOCK_THREAD_CPUTIME_ID" : "unavailable");
    }

    if (!skip_sws && !p->fpga_yuv420 && !*sws) {
        *sws = sws_getContext(FB_W, player_active_h(p), frame->format,
                              FB_W, player_active_h(p), AV_PIX_FMT_BGR0,
                              SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!*sws) {
            fprintf(stderr, "sws_getContext failed\n");
            player_abort(p);
            return -1;
        }
    }

    int64_t prev_mbox_us = p->last_present_end_us;
    int64_t cycle_t0 = av_gettime_relative();
    int64_t ack_wait_us = 0;
    int ack_instant = 0;
    int ack_waited = 0;
    if (p->flips > 0) {
        if (wait_display_buf(p, p->displayed, ACK_TIMEOUT_US,
                             &ack_wait_us, &ack_instant) < 0) {
            player_abort(p);
            return -1;
        }
        ack_waited = 1;
        note_display_ack(p, av_gettime_relative());
    }

    int on_screen;
    if (wait_valid_display_buf(p, &on_screen, ACK_TIMEOUT_US) < 0) {
        player_abort(p);
        return -1;
    }

    int next = on_screen ^ 1;
    if (next == on_screen) {
        fprintf(stderr, "FAIL: dest buffer is display_buf\n");
        player_abort(p);
        return -1;
    }
    if (p->flips > 0 && next == p->displayed) {
        fprintf(stderr, "FAIL: dest is the unacknowledged mailbox request\n");
        player_abort(p);
        return -1;
    }

    int64_t own0 = av_gettime_relative();
    dst_data[0] = next ? p->fb->fb_b : p->fb->fb_a;
    dst_linesize[0] = FB_STRIDE;
    int64_t cpu0 = p->sws_cpu_ok ? thread_cpu_us() : -1;
    int64_t c0 = av_gettime_relative();
    int64_t sws_wall = 0;
    int64_t cpu1 = -1;
    int sub_active = 0;
    int64_t sub_blend_us = 0;

    if (p->fpga_yuv420) {
        log_yuv_frame_meta_once(p, frame);
        phase_sws_enter(p);
        if (copy_yuv420_to_slot(dst_data[0], frame, player_active_h(p)) < 0) {
            fprintf(stderr, "FAIL: FPGA YUV420 plane copy\n");
            player_abort(p);
            return -1;
        }
        sws_wall = av_gettime_relative() - c0;
        cpu1 = (p->sws_cpu_ok && cpu0 >= 0) ? thread_cpu_us() : -1;
        p->convert_us += sws_wall;
        yuv_copy_note(p, sws_wall);
        phase_sws_leave(p);
    } else if (!skip_sws) {
        phase_sws_enter(p);
        sws_scale(*sws, (const uint8_t * const *)frame->data, frame->linesize,
                  0, player_active_h(p), dst_data, dst_linesize);
        sws_wall = av_gettime_relative() - c0;
        cpu1 = (p->sws_cpu_ok && cpu0 >= 0) ? thread_cpu_us() : -1;
        p->convert_us += sws_wall;
        {
            int64_t ov0 = av_gettime_relative();
            int64_t ov_us;

            if (frame_menu || p->in_menu || g_debug_yellow_highlight)
                present_draw_highlight(p, dst_data[0], dst_linesize[0],
                                       frame_menu);
            else
                sub_active = movie_sub_overlay(p, dst_data[0],
                                               dst_linesize[0], vpts_us,
                                               &sub_blend_us);
            ov_us = av_gettime_relative() - ov0;
            if (frame_menu || p->in_menu) {
                p->spu_perf.menu_frames++;
                p->spu_perf.sws_sum += sws_wall;
                p->spu_perf.ov_sum += ov_us;
                if (ov_us > p->spu_perf.ov_max)
                    p->spu_perf.ov_max = ov_us;
                if (ui_redraw) {
                    p->spu_perf.still_redraws++;
                    p->spu_perf.still_sum += sws_wall + ov_us;
                }
            }
        }
        phase_sws_leave(p);
    }
    __sync_synchronize();
    int64_t conv_done = av_gettime_relative();

    int64_t pts_wait0 = conv_done;
    if (!skip_clock && pvpts_us != AV_NOPTS_VALUE)
        present_wait_pts(p, pvpts_us);
    int64_t post_sws_wait_us = av_gettime_relative() - pts_wait0;
    if (post_sws_wait_us < 0)
        post_sws_wait_us = 0;

    int64_t aclk = clock_read(&p->clock, NULL, NULL);
    int64_t av_off = 0;
    if (aclk != AV_NOPTS_VALUE && vpts_us != AV_NOPTS_VALUE) {
        int64_t raw_off = vpts_us - aclk;
        av_off = (pvpts_us != AV_NOPTS_VALUE) ? pvpts_us - aclk : raw_off;
        record_offset_pair(p, raw_off, av_off);
    }

    int64_t mbox_t0 = av_gettime_relative();
    p->fb->mbox[0] = mailbox_ab_word(p->fpga_yuv420, next);
    p->last_mbox_wall_us = av_gettime_relative();
    int64_t mbox_write_us = p->last_mbox_wall_us - mbox_t0;
    if (mbox_write_us < 0)
        mbox_write_us = 0;
    int display_after = on_screen;
    {
        int after_cur = on_screen;
        if (read_display_buf(p->fb, &after_cur) == 0)
            display_after = after_cur;
    }
    uint64_t status_word = peek_mbox_status(p->fb);
    if (next)
        p->frames_b++;
    else
        p->frames_a++;
    p->flips++;
    p->displayed = next;
    p->rendered++;
    p->stale_run = 0;
    p->last_present_end_us = p->last_mbox_wall_us;
    if (!skip_clock)
        pause_note_resume_video_pts(p, vpts_us);

    {
        int64_t a2m = p->last_mbox_wall_us - own0;
        int64_t c2m = p->last_mbox_wall_us - conv_done;
        int64_t cycle = p->last_mbox_wall_us - cycle_t0;
        int64_t sws_cpu = -1, preempt = -1;

        if (a2m < 0)
            a2m = 0;
        if (c2m < 0)
            c2m = 0;
        if (cycle < 0)
            cycle = 0;

        if (ack_waited) {
            stat_add(&p->ackw_n, &p->ackw_sum, &p->ackw_min, &p->ackw_max,
                     ack_wait_us);
            if (ack_instant)
                p->ackw_instant++;
            if (ack_wait_us > 5000)
                p->ackw_gt5++;
            if (ack_wait_us > 10000)
                p->ackw_gt10++;
            if (ack_wait_us > 20000)
                p->ackw_gt20++;
            if (ack_wait_us > 30000)
                p->ackw_gt30++;
            if (ack_wait_us > 40000)
                p->ackw_gt40++;
        }

        if (!skip_sws) {
            stat_add(&p->sws_n, &p->sws_sum, &p->sws_min, &p->sws_max, sws_wall);
            if (sws_wall > 20000)
                p->sws_gt20++;
            if (sws_wall > 25000)
                p->sws_gt25++;
            if (sws_wall > 30000)
                p->sws_gt30++;
            if (sws_wall > 35000)
                p->sws_gt35++;
            if (sws_wall > 40000)
                p->sws_gt40++;
            if (sws_wall > 50000)
                p->sws_gt50++;
        }
        if (p->sws_cpu_ok && cpu0 >= 0 && cpu1 >= 0) {
            sws_cpu = cpu1 - cpu0;
            if (sws_cpu < 0)
                sws_cpu = 0;
            preempt = sws_wall - sws_cpu;
            if (preempt < 0)
                preempt = 0;
            stat_add(&p->sws_cpu_n, &p->sws_cpu_sum, &p->sws_cpu_min,
                     &p->sws_cpu_max, sws_cpu);
            if (p->sws_cpu_n == 1) {
                p->preempt_min = preempt;
                p->preempt_max = preempt;
                p->preempt_sum = preempt;
            } else {
                p->preempt_sum += preempt;
                if (preempt < p->preempt_min)
                    p->preempt_min = preempt;
                if (preempt > p->preempt_max)
                    p->preempt_max = preempt;
            }
            if (preempt > 1000)
                p->preempt_gt1++;
            if (preempt > 2000)
                p->preempt_gt2++;
            if (preempt > 5000)
                p->preempt_gt5++;
            if (preempt > 10000)
                p->preempt_gt10++;
        }

        if (prev_mbox_us > 0) {
            int64_t mc = p->last_mbox_wall_us - prev_mbox_us;
            stat_add(&p->mbox_cyc_n, &p->mbox_cyc_sum, &p->mbox_cyc_min,
                     &p->mbox_cyc_max, mc);
        }

        stat_add(&p->a2m_n, &p->a2m_sum, &p->a2m_min, &p->a2m_max, a2m);
        if (a2m > 20000)
            p->a2m_gt20++;
        if (a2m > 30000)
            p->a2m_gt30++;
        if (a2m > 35000)
            p->a2m_gt35++;
        if (a2m > 40000)
            p->a2m_gt40++;
        if (a2m > 50000)
            p->a2m_gt50++;
        stat_add(&p->c2m_n, &p->c2m_sum, &p->c2m_min, &p->c2m_max, c2m);
        stat_add(&p->cyc_n, &p->cyc_sum, &p->cyc_min, &p->cyc_max, cycle);
        if (cycle > 30000)
            p->cyc_gt30++;
        if (cycle > 35000)
            p->cyc_gt35++;
        if (cycle > 40000)
            p->cyc_gt40++;
        if (cycle > 50000)
            p->cyc_gt50++;
        if (cycle > 80000)
            p->cyc_gt80++;

        p->last_path.valid = 1;
        p->last_path.frame = p->rendered;
        p->last_path.vpts_us = vpts_us;
        p->last_path.av_off_us = av_off;
        p->last_path.ack_wait_us = ack_wait_us;
        p->last_path.ack_instant = ack_instant;
        p->last_path.ack_waited = ack_waited;
        p->last_path.decode_us = p->cur_decode_us;
        p->last_path.sws_wall_us = sws_wall;
        p->last_path.sws_cpu_us = sws_cpu;
        p->last_path.preempt_us = preempt;
        p->last_path.ack_to_mbox_us = a2m;
        p->last_path.conv_to_mbox_us = c2m;
        p->last_path.cycle_us = cycle;
        iso_finish_warmup_or_record(p, ack_wait_us, ack_waited, cycle);
        phase_note_present(p, skip_sws ? 0 : sws_wall, ack_wait_us, ack_waited,
                           cycle);
        subperf_note_present(p, sub_active, sub_blend_us,
                             skip_sws ? 0 : sws_wall, ack_wait_us, ack_waited,
                             cycle);
    }

    if (!p->perf_present_no_convert && !skip_clock && p->rendered > 0 &&
        (p->rendered % PRESENT_PERF_INTERVAL) == 0) {
        log_present_perf(p, vpts_us, pvpts_us, decision_aclk, decision_delta,
                         ack_wait_us, ack_instant, sws_wall, post_sws_wait_us,
                         p->last_path.cycle_us,
                         ack_waited ? p->last_mbox_to_ack_us : -1,
                         mbox_write_us, on_screen, next, display_after,
                         status_word);
    }
    return 0;
}

static void subperf_print_dist(const char *name, int64_t *v, int n_all,
                               int64_t sum, int64_t maxv)
{
    int n = n_all < ISO_SAMPLE_CAP ? n_all : ISO_SAMPLE_CAP;
    int64_t mean = n_all > 0 ? sum / n_all : 0;
    int64_t p50 = 0, p90 = 0, p95 = 0, p99 = 0, mx = maxv;

    if (n > 0 && v) {
        qsort(v, (size_t)n, sizeof(int64_t), cmp_i64);
        p50 = sorted_pct(v, n, 50);
        p90 = sorted_pct(v, n, 90);
        p95 = sorted_pct(v, n, 95);
        p99 = sorted_pct(v, n, 99);
        mx = v[n - 1];
    }
    fprintf(stderr,
            "  %s mean=%" PRId64 " p50=%" PRId64 " p90=%" PRId64
            " p95=%" PRId64 " p99=%" PRId64 " max=%" PRId64 "\n",
            name, mean, p50, p90, p95, p99, mx);
}

static void *present_thread(void *opaque)
{
    Player *p = opaque;
    int64_t t0 = av_gettime_relative();
    int playing = 0;
    struct SwsContext *sws = NULL;
    AVFrame *menu_hold = NULL;
    unsigned seen_gen;
    unsigned menu_hold_gen = 0;

    note_unpinned_cpu("present", &p->sched_present_cpu);

    menu_hold = av_frame_alloc();
    prefill_wait(p);
    if (p->fail || g_interrupt)
        goto done;
    if (!p->in_menu)
        clock_wait_ready(p);
    if (p->fail || g_interrupt)
        goto done;

    p->buf_playing = 1;
    playing = 1;
    if (p->phase_decode && !p->phase_t0_us)
        p->phase_t0_us = av_gettime_relative();
    seen_gen = player_nav_gen(p);
    dbg("presentation consumer released\n");
    dbg("Audio ring ready — buffered consumer starting presentation.\n");

    for (;;) {
        int pr = 0;
        int64_t wait0 = av_gettime_relative();
        unsigned g = player_nav_gen(p);
        int redraw = 0;

        if (g != seen_gen) {
            seen_gen = g;
            if (p->in_menu) {
                dbg("DVD MENU: presentation follows menu gen=%u\n", g);
            } else {
                av_frame_unref(menu_hold);
                menu_hold_gen = 0;
                p->menu_still_drop = 0;
                prefill_wait(p);
                if (p->fail || g_interrupt)
                    break;
                clock_wait_ready(p);
                if (p->fail || g_interrupt)
                    break;
                dbg("DVD MENU: presentation resumes title gen=%u\n", g);
            }
        }

        if (p->menu_still_drop) {
            av_frame_unref(menu_hold);
            menu_hold_gen = 0;
            p->menu_still_drop = 0;
        }

        if (p->hl.inited) {
            pthread_mutex_lock(&p->hl.mu);
            redraw = p->hl.redraw;
            if (redraw)
                p->hl.redraw = 0;
            pthread_mutex_unlock(&p->hl.mu);
        }
        if (redraw && menu_hold && menu_hold->data[0] &&
            p->in_menu && menu_hold_gen == player_nav_gen(p)) {
            p->cur_decode_us = 0;
            pr = present_yuv_frame(p, menu_hold, AV_NOPTS_VALUE, &sws, 1, 1);
            p->menu_redraws++;
            if (pr < 0)
                break;
            continue;
        }

        if (pause_is_held(p)) {
            pause_thr_set(&p->pause.st_present, THR_PAUSE);
            pause_wait_unheld(p);
            pause_thr_set(&p->pause.st_present, THR_RUN);
            if (p->fail || g_interrupt)
                break;
            continue;
        }

        if (p->buffered_yuv) {
            AVFrame *yf = NULL;
            int64_t vpts_us = AV_NOPTS_VALUE;
            unsigned slot_gen = 0;
            int slot_menu = 0;
            int acq = yuvring_acquire_filled(&p->yuvring, &yf, &vpts_us,
                                             &slot_gen, &slot_menu, playing);
            if (acq == 0)
                break;
            if (acq == 2)
                continue;
            p->cur_decode_us = av_gettime_relative() - wait0;
            if (p->cur_decode_us < 0)
                p->cur_decode_us = 0;
            if (slot_gen != player_nav_gen(p)) {
                yuvring_release(&p->yuvring, playing);
                continue;
            }
            pr = present_yuv_frame(p, yf, vpts_us, &sws, 0, slot_menu);
            if (pr == 0 && slot_menu && yf) {
                av_frame_unref(menu_hold);
                av_frame_ref(menu_hold, yf);
                menu_hold_gen = slot_gen;
            } else if (!slot_menu) {
                av_frame_unref(menu_hold);
                menu_hold_gen = 0;
            }
            yuvring_release(&p->yuvring, playing);
        } else {
            int idx = 0;
            int64_t vpts_us = AV_NOPTS_VALUE;
            uint8_t *pix = NULL;

            if (!vidring_acquire_filled(&p->vring, &idx, &vpts_us, &pix, playing))
                break;
            p->cur_decode_us = av_gettime_relative() - wait0;
            if (p->cur_decode_us < 0)
                p->cur_decode_us = 0;
            pr = present_cached_frame(p, pix, vpts_us);
            vidring_release(&p->vring, playing);
        }
        if (pr < 0)
            break;
        if (p->fail || g_interrupt)
            break;
    }

done:
    if (menu_hold)
        av_frame_free(&menu_hold);
    if (sws)
        sws_freeContext(sws);
    p->present_cpu_us = av_gettime_relative() - t0;
    return NULL;
}

static int video_trace_active(const Player *p)
{
    return p->soft_decode_trace || p->still_drain_req;
}

static int video_emit_frame(Player *p, AVFrame *frame, AVCodecContext *vdec,
                            struct SwsContext **sws, unsigned pkt_gen,
                            int timed)
{
    __atomic_add_fetch(&p->frames_this_nav_gen, 1, __ATOMIC_SEQ_CST);
    if (p->soft_log_decode && pkt_gen == p->soft_nav_gen) {
        p->soft_log_decode = 0;
        dbg("DVD MENU: first decoded video frame gen=%u pts=%" PRId64 "\n",
            pkt_gen, frame->pts);
    }
    if (p->uncapped_bench) {
        int64_t now = av_gettime_relative();
        int64_t decode_us = (p->bench_last_mark_us > 0)
                            ? now - p->bench_last_mark_us : -1;
        if (bench_convert_frame(p, frame, vdec, sws, decode_us) < 0)
            return -1;
        return 0;
    }
    if (p->buffered_yuv) {
        if (p->initial_skip_left > 0 && !p->in_menu) {
            dbg("DVD MENU: decoded frame skipped (initial-video-skip) "
                "gen=%u left=%d\n",
                pkt_gen, p->initial_skip_left);
            p->initial_skip_left--;
            p->initial_video_skipped++;
            return 0;
        }
        {
            int64_t now = av_gettime_relative();
            int64_t decode_us = (p->prod_last_end_us > 0)
                                ? now - p->prod_last_end_us : -1;
            int er = producer_enqueue_yuv(p, frame, vdec, decode_us, pkt_gen);
            if (er != 0)
                return -1;
        }
        return 0;
    }
    if (p->buffered_video) {
        if (p->initial_skip_left > 0) {
            p->initial_skip_left--;
            p->initial_video_skipped++;
            return 0;
        }
        {
            int64_t now = av_gettime_relative();
            int64_t decode_us = (p->prod_last_end_us > 0)
                                ? now - p->prod_last_end_us : -1;
            int er = producer_enqueue_frame(p, frame, vdec, sws, decode_us);
            if (er != 0)
                return -1;
        }
        return 0;
    }
    if (!timed) {
        p->preroll_decoded++;
        return 0;
    }
    if (p->initial_skip_left > 0) {
        p->initial_skip_left--;
        p->initial_video_skipped++;
        return 0;
    }
    if (p->last_present_end_us > 0) {
        int64_t decode_us =
            av_gettime_relative() - p->last_present_end_us;
        if (decode_us < 0)
            decode_us = 0;
        p->cur_decode_us = decode_us;
    } else {
        p->cur_decode_us = -1;
    }
    p->video_decoded++;
    {
        int pr = present_video_frame(p, frame, vdec, sws, 1);
        if (pr < 0)
            return -1;
        if (pr == 0 && p->cur_decode_us >= 0) {
            int64_t decode_us = p->cur_decode_us;
            stat_add(&p->dec_n, &p->dec_sum, &p->dec_min,
                     &p->dec_max, decode_us);
            if (decode_us > 10000)
                p->dec_gt10++;
            if (decode_us > 20000)
                p->dec_gt20++;
            if (decode_us > 30000)
                p->dec_gt30++;
            if (decode_us > 40000)
                p->dec_gt40++;
        }
    }
    return 0;
}

static int video_send_and_receive(Player *p, AVCodecContext *vdec,
                                  AVPacket *ppkt, AVFrame *frame,
                                  struct SwsContext **sws, unsigned pkt_gen,
                                  int timed, int trace, const char *src)
{
    char errbuf[64];
    int sr, rr;

    phase_decode_begin(p);
    sr = avcodec_send_packet(vdec, ppkt);
    if (trace) {
        if (sr < 0)
            fferr(sr, errbuf, sizeof(errbuf));
        dbg("DVD MENU: avcodec_send_packet src=%s ret=%d %s  size=%d "
            "pts=%" PRId64 " dts=%" PRId64 "\n",
            src, sr, sr < 0 ? errbuf : "ok",
            ppkt ? ppkt->size : 0,
            ppkt ? ppkt->pts : AV_NOPTS_VALUE,
            ppkt ? ppkt->dts : AV_NOPTS_VALUE);
    }
    if (sr < 0) {
        phase_decode_end(p);
        if (p->uncapped_bench)
            p->bench_decode_errors++;
        return 0;
    }
    while ((rr = avcodec_receive_frame(vdec, frame)) == 0) {
        if (trace)
            dbg("DVD MENU: avcodec_receive_frame ret=0 pts=%" PRId64
                " gen=%u\n",
                frame->pts, pkt_gen);
        phase_decode_end(p);
        if (player_nav_gen(p) != pkt_gen) {
            dbg("DVD MENU: decoded frame rejected (nav_gen %u -> %u)\n",
                pkt_gen, player_nav_gen(p));
            av_frame_unref(frame);
            phase_decode_begin(p);
            continue;
        }
        if (video_emit_frame(p, frame, vdec, sws, pkt_gen, timed) < 0)
            return -1;
        av_frame_unref(frame);
        phase_decode_begin(p);
    }
    phase_decode_end(p);
    if (trace) {
        if (rr == AVERROR(EAGAIN))
            dbg("DVD MENU: avcodec_receive_frame ret=EAGAIN src=%s\n", src);
        else if (rr == AVERROR_EOF)
            dbg("DVD MENU: avcodec_receive_frame ret=EOF src=%s\n", src);
        else if (rr < 0) {
            fferr(rr, errbuf, sizeof(errbuf));
            dbg("DVD MENU: avcodec_receive_frame ret=%d %s src=%s\n",
                rr, errbuf, src);
        }
    }
    return 0;
}

static int video_drain_still_boundary(Player *p, AVCodecParserContext *parser,
                                      AVCodecContext *vdec, AVPacket *ppkt,
                                      AVFrame *frame, struct SwsContext **sws,
                                      unsigned pkt_gen, int timed)
{
    int i, used, out_size, before;
    uint8_t *out_data = NULL;
    unsigned frames0 =
        __atomic_load_n(&p->frames_this_nav_gen, __ATOMIC_SEQ_CST);

    dbg("DVD MENU: STILL-BOUNDARY drain start  nav_gen=%u codec_gen=%u "
        "frames_this_gen=%u parser_off=%" PRId64 "/%" PRId64 "\n",
        pkt_gen, player_codec_gen(p), frames0,
        parser ? parser->frame_offset : -1,
        parser ? parser->cur_offset : -1);

    /* Drain is not reset: zero-size parser feed emits a held final AU. */
    for (i = 0; i < 8 && parser; i++) {
        out_data = NULL;
        out_size = 0;
        used = av_parser_parse2(parser, vdec, &out_data, &out_size,
                                NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        dbg("DVD MENU: parser drain[%d] used=%d out=%d retained=%s "
            "off=%" PRId64 "/%" PRId64 "\n",
            i, used, out_size,
            (used == 0 && out_size == 0) ? "no" : "yes",
            parser->frame_offset, parser->cur_offset);
        if (used < 0)
            break;
        if (out_size > 0) {
            ppkt->data = out_data;
            ppkt->size = out_size;
            ppkt->pts = parser->pts;
            ppkt->dts = parser->dts;
            if (video_send_and_receive(p, vdec, ppkt, frame, sws, pkt_gen,
                                       timed, 1, "parser-drain") < 0)
                return -1;
        }
        if (used <= 0 && out_size <= 0)
            break;
    }

    before = (int)__atomic_load_n(&p->frames_this_nav_gen, __ATOMIC_SEQ_CST);
    if (before == (int)frames0) {
        /* Parser emitted nothing. Drain codec without using flush as drain. */
        dbg("DVD MENU: codec drain (send NULL) — parser held no AU\n");
        if (video_send_and_receive(p, vdec, NULL, frame, sws, pkt_gen,
                                   timed, 1, "codec-drain") < 0)
            return -1;
        /* Leave draining/EOF so later packets of this generation work. */
        avcodec_flush_buffers(vdec);
        dbg("DVD MENU: codec resumed after drain (flush_buffers post-EOF, "
            "not used as drain)\n");
    }

    pthread_mutex_lock(&p->prefill_mu);
    p->still_drain_done = 1;
    p->still_drain_req = 0;
    pthread_cond_broadcast(&p->prefill_cv);
    pthread_mutex_unlock(&p->prefill_mu);
    dbg("DVD MENU: STILL-BOUNDARY drain done  frames_this_gen=%u "
        "yuv=%d\n",
        __atomic_load_n(&p->frames_this_nav_gen, __ATOMIC_SEQ_CST),
        p->buffered_yuv ? yuvring_count(&p->yuvring) : -1);
    return 0;
}

static void *video_thread(void *opaque)
{
    Player *p = opaque;
    int64_t t0 = av_gettime_relative();
    AVCodecContext *vdec = NULL;
    AVCodecParserContext *parser = NULL;
    struct SwsContext *sws = NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    AVPacket *ppkt = av_packet_alloc();
    int timed = 0;
    int ready_logged = 0;

    note_unpinned_cpu("video", &p->sched_video_cpu);

    p->present_vbl = -1;
    p->first_genuine_pts = AV_NOPTS_VALUE;
    p->timeline_pts = AV_NOPTS_VALUE;
    p->assigned_pts = AV_NOPTS_VALUE;
    p->last_video_pts_us = AV_NOPTS_VALUE;
    p->cur_decode_us = -1;
    p->sws_cpu_ok = thread_cpu_us() >= 0;
    if (!p->sws_cpu_ok)
        dbg("CLOCK_THREAD_CPUTIME_ID unavailable — omitting sws CPU/"
            "preemption stats.\n");

    if (!frame || !pkt || !ppkt) {
        player_abort(p);
        goto done;
    }
    vdec = open_decoder(p->vcp, p->vtb, AVMEDIA_TYPE_VIDEO);
    parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    if (!vdec || !parser) {
        fprintf(stderr, "video decoder/parser init failed\n");
        player_abort(p);
        goto done;
    }

    if (p->uncapped_bench) {
        p->bench_video_cpu0 = thread_cpu_us();
        if (bench_select_inactive_ddr(p) < 0) {
            player_abort(p);
            goto done;
        }
    }
    if (player_buffered(p) && p->initial_skip_left > 0)
        dbg("Initial video skip: discarding %d decoded frame%s "
            "before %s queue.\n",
            p->initial_skip_left,
            p->initial_skip_left == 1 ? "" : "s",
            p->buffered_yuv ? "decoded-YUV" : "cached");

    unsigned dec_nav_gen = player_nav_gen(p);
    unsigned dec_codec_gen = player_codec_gen(p);

    while (pktq_pop(&p->vq, pkt)) {
        const uint8_t *in = pkt->data;
        int in_size = pkt->size;
        int64_t in_pts = pkt->pts, in_dts = pkt->dts, in_pos = pkt->pos;
        unsigned pkt_gen = player_nav_gen(p);
        unsigned pkt_codec = player_codec_gen(p);
        int trace = video_trace_active(p);

        if (pkt->opaque == VQ_MARK_STILL_BOUNDARY) {
            dbg("DVD MENU: STILL-BOUNDARY marker popped  nav_gen=%u "
                "codec_gen=%u\n",
                pkt_gen, pkt_codec);
            av_packet_unref(pkt);
            if (video_drain_still_boundary(p, parser, vdec, ppkt, frame, &sws,
                                           pkt_gen, timed) < 0)
                break;
            continue;
        }

        if (pkt_codec != dec_codec_gen) {
            dec_codec_gen = pkt_codec;
            dec_nav_gen = pkt_gen;
            p->video_reset_req = 0;
            avcodec_flush_buffers(vdec);
            av_parser_close(parser);
            parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
            if (!parser) {
                fprintf(stderr, "video parser re-init failed\n");
                av_packet_unref(pkt);
                player_abort(p);
                break;
            }
            p->first_genuine_pts = AV_NOPTS_VALUE;
            p->timeline_pts = AV_NOPTS_VALUE;
            p->assigned_pts = AV_NOPTS_VALUE;
            dbg("DVD MENU: video decoder/parser HARD reset  "
                "codec_gen=%u nav_gen=%u\n",
                pkt_codec, pkt_gen);
        } else if (pkt_gen != dec_nav_gen) {
            dbg("DVD MENU: presentation gen %u -> %u  "
                "codec_gen=%u unchanged (parser/decoder preserved)\n",
                dec_nav_gen, pkt_gen, pkt_codec);
            dec_nav_gen = pkt_gen;
        }

        if (trace)
            dbg("DVD MENU: video packet  gen=%u codec_gen=%u size=%d "
                "pts=%" PRId64 " dts=%" PRId64 "\n",
                pkt_gen, pkt_codec, in_size, in_pts, in_dts);

        if (!timed && !player_buffered(p)) {
            pthread_mutex_lock(&p->clock.mu);
            timed = p->clock.ready;
            pthread_mutex_unlock(&p->clock.mu);
            if (timed && !ready_logged) {
                dbg("Audio ring ready — starting timed video.\n");
                if (p->initial_skip_left > 0)
                    dbg("Initial video skip: discarding %d decoded frame%s "
                        "before first present.\n",
                        p->initial_skip_left,
                        p->initial_skip_left == 1 ? "" : "s");
                ready_logged = 1;
            }
        }

        while (in_size > 0 && !p->fail) {
            uint8_t *out_data = NULL;
            int out_size = 0;
            int used;
            int in_before = in_size;

            if (player_nav_gen(p) != pkt_gen)
                break;
            used = av_parser_parse2(parser, vdec, &out_data, &out_size,
                                        in, in_size, in_pts, in_dts, in_pos);
            if (trace)
                dbg("DVD MENU: parser in=%d used=%d out=%d retained=%s "
                    "off=%" PRId64 "/%" PRId64 "\n",
                    in_before, used, out_size,
                    (used > 0 && out_size <= 0) ? "yes" : "no",
                    parser->frame_offset, parser->cur_offset);
            if (used < 0)
                break;
            in += used;
            in_size -= used;
            in_pts = AV_NOPTS_VALUE;
            in_dts = AV_NOPTS_VALUE;
            in_pos = -1;
            if (out_size <= 0)
                continue;
            ppkt->data = out_data;
            ppkt->size = out_size;
            ppkt->pts = parser->pts;
            ppkt->dts = parser->dts;
            if (video_send_and_receive(p, vdec, ppkt, frame, &sws, pkt_gen,
                                       timed, trace, "parse") < 0) {
                av_packet_unref(pkt);
                goto done;
            }
        }
        av_packet_unref(pkt);
    }

done:
    if (p->buffered_yuv) {
        yuvring_eof(&p->yuvring);
        if (!prefill_is_released(p))
            prefill_release(p, p->fail ? "producer abort"
                                      : "producer EOF / title end before prefill");
    } else if (p->buffered_video) {
        vidring_eof(&p->vring);
        if (!prefill_is_released(p))
            prefill_release(p, p->fail ? "producer abort"
                                      : "producer EOF / title end before prefill");
    }
    p->video_cpu_us = av_gettime_relative() - t0;
    if (p->uncapped_bench) {
        int64_t cpu1 = thread_cpu_us();
        if (p->bench_video_cpu0 >= 0 && cpu1 >= 0)
            p->bench_video_cpu_us = cpu1 - p->bench_video_cpu0;
        p->bench_mbox0_end = p->fb->mbox[0];
        if (read_display_buf(p->fb, &p->bench_display_end) != 0)
            p->bench_display_end = -1;
    }
    if (sws)
        sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_packet_free(&ppkt);
    if (parser)
        av_parser_close(parser);
    avcodec_free_context(&vdec);
    return NULL;
}

static void print_perf_isolation_ack(const Player *p)
{
    int64_t elapsed, decoded, presented, stale;
    double secs;
    int ack_n, cyc_n;
    int64_t ack_mean = 0, cyc_mean = 0;
    int64_t ack_med = 0, ack_p95 = 0, ack_p99 = 0, ack_max = 0;
    int64_t cyc_p95 = 0, cyc_p99 = 0, cyc_max = 0;

    fprintf(stderr, "\nPERF ISOLATION ACK:\n");
    if (!p->iso_started || p->iso_t0_us <= 0) {
        fprintf(stderr,
                "  elapsed=0 (A/B warmup did not complete; presented=%d)\n",
                p->rendered);
        return;
    }
    elapsed = av_gettime_relative() - p->iso_t0_us;
    if (elapsed < 0)
        elapsed = 0;
    secs = elapsed / 1e6;
    decoded = (int64_t)p->video_decoded - p->iso_decoded0;
    if (decoded < 0)
        decoded = 0;
    presented = p->iso_n;
    stale = (int64_t)p->stale_dropped - p->iso_stale0;
    if (stale < 0)
        stale = 0;
    ack_n = p->iso_ack_n < ISO_SAMPLE_CAP ? p->iso_ack_n : ISO_SAMPLE_CAP;
    cyc_n = p->iso_n < ISO_SAMPLE_CAP ? p->iso_n : ISO_SAMPLE_CAP;
    if (p->iso_ack_n > 0)
        ack_mean = p->iso_ack_sum / p->iso_ack_n;
    if (p->iso_n > 0)
        cyc_mean = p->iso_cyc_sum / p->iso_n;
    if (ack_n > 0 && p->iso_ack) {
        qsort(p->iso_ack, (size_t)ack_n, sizeof(int64_t), cmp_i64);
        ack_med = sorted_pct(p->iso_ack, ack_n, 50);
        ack_p95 = sorted_pct(p->iso_ack, ack_n, 95);
        ack_p99 = sorted_pct(p->iso_ack, ack_n, 99);
        ack_max = p->iso_ack[ack_n - 1];
    }
    if (cyc_n > 0 && p->iso_cyc) {
        qsort(p->iso_cyc, (size_t)cyc_n, sizeof(int64_t), cmp_i64);
        cyc_p95 = sorted_pct(p->iso_cyc, cyc_n, 95);
        cyc_p99 = sorted_pct(p->iso_cyc, cyc_n, 99);
        cyc_max = p->iso_cyc[cyc_n - 1];
    }
    fprintf(stderr,
            "  elapsed=%" PRId64 "\n"
            "  decoded=%" PRId64 "\n"
            "  presented=%" PRId64 "\n"
            "  stale=%" PRId64 "\n"
            "  effective_present_fps=%.3f\n"
            "  stale_fps=%.3f\n"
            "  ack mean=%" PRId64 "\n"
            "  ack median=%" PRId64 "\n"
            "  ack p95=%" PRId64 "\n"
            "  ack p99=%" PRId64 "\n"
            "  ack max=%" PRId64 "\n"
            "  cycle mean=%" PRId64 "\n"
            "  cycle p95=%" PRId64 "\n"
            "  cycle p99=%" PRId64 "\n"
            "  cycle max=%" PRId64 "\n"
            "  ack >20ms count=%lu\n"
            "  ack >30ms count=%lu\n"
            "  ack >40ms count=%lu\n",
            elapsed, decoded, presented, stale,
            secs > 0.0 ? presented / secs : 0.0,
            secs > 0.0 ? stale / secs : 0.0,
            ack_mean, ack_med, ack_p95, ack_p99, ack_max,
            cyc_mean, cyc_p95, cyc_p99, cyc_max,
            p->iso_ack_gt20, p->iso_ack_gt30, p->iso_ack_gt40);
}

static void print_phase_decode(Player *p)
{
    int64_t elapsed;
    double secs;
    int sws_n, ack_n, cyc_n;
    int64_t sws_mean = 0, sws_p95 = 0, sws_p99 = 0;
    int64_t ack_mean = 0, ack_p95 = 0;
    int64_t cyc_mean = 0, cyc_p95 = 0, cyc_p99 = 0;
    const YuvRing *r = &p->yuvring;
    double qavg = 0.0;

    fprintf(stderr, "\n=== PHASE DECODE ===\n");
    if (p->phase_t0_us <= 0) {
        fprintf(stderr, "elapsed=0 (presentation did not start)\n");
        return;
    }
    elapsed = av_gettime_relative() - p->phase_t0_us;
    if (elapsed < 0)
        elapsed = 0;
    secs = elapsed / 1e6;
    if (r->play_depth_n)
        qavg = (double)r->play_depth_sum / r->play_depth_n;
    sws_n = p->phase_sws_n < ISO_SAMPLE_CAP ? p->phase_sws_n : ISO_SAMPLE_CAP;
    ack_n = p->phase_ack_n < ISO_SAMPLE_CAP ? p->phase_ack_n : ISO_SAMPLE_CAP;
    cyc_n = p->phase_cyc_n < ISO_SAMPLE_CAP ? p->phase_cyc_n : ISO_SAMPLE_CAP;
    if (p->phase_sws_n > 0)
        sws_mean = p->phase_sws_sum / p->phase_sws_n;
    if (p->phase_ack_n > 0)
        ack_mean = p->phase_ack_sum / p->phase_ack_n;
    if (p->phase_cyc_n > 0)
        cyc_mean = p->phase_cyc_sum / p->phase_cyc_n;
    if (sws_n > 0 && p->phase_sws) {
        qsort(p->phase_sws, (size_t)sws_n, sizeof(int64_t), cmp_i64);
        sws_p95 = sorted_pct(p->phase_sws, sws_n, 95);
        sws_p99 = sorted_pct(p->phase_sws, sws_n, 99);
    }
    if (ack_n > 0 && p->phase_ack) {
        qsort(p->phase_ack, (size_t)ack_n, sizeof(int64_t), cmp_i64);
        ack_p95 = sorted_pct(p->phase_ack, ack_n, 95);
    }
    if (cyc_n > 0 && p->phase_cyc) {
        qsort(p->phase_cyc, (size_t)cyc_n, sizeof(int64_t), cmp_i64);
        cyc_p95 = sorted_pct(p->phase_cyc, cyc_n, 95);
        cyc_p99 = sorted_pct(p->phase_cyc, cyc_n, 99);
    }
    fprintf(stderr,
            "elapsed=%" PRId64 "\n"
            "presented fps=%.3f\n"
            "stale fps=%.3f\n"
            "queue min/avg/max=%d / %.1f / %d\n"
            "sws mean=%" PRId64 "\n"
            "sws p95=%" PRId64 "\n"
            "sws p99=%" PRId64 "\n"
            "ACK mean=%" PRId64 "\n"
            "ACK p95=%" PRId64 "\n"
            "combined cycle mean=%" PRId64 "\n"
            "combined cycle p95=%" PRId64 "\n"
            "combined cycle p99=%" PRId64 "\n"
            "producer_decode_overlap_with_sws count=%lu\n"
            "phase waits=%lu  bypass low-water=%lu  bypass vq-high=%lu\n",
            elapsed,
            secs > 0.0 ? p->rendered / secs : 0.0,
            secs > 0.0 ? p->stale_dropped / secs : 0.0,
            r->play_depth_n ? r->play_depth_min : 0, qavg,
            r->depth_max,
            sws_mean, sws_p95, sws_p99,
            ack_mean, ack_p95,
            cyc_mean, cyc_p95, cyc_p99,
            p->phase_overlap_n,
            p->phase_wait_n, p->phase_bypass_low, p->phase_bypass_vq);
}

static void fill_synth_yuv420p_pal(AVFrame *f)
{
    int x, y;
    int ls_y = f->linesize[0];
    int ls_u = f->linesize[1];
    int ls_v = f->linesize[2];

    for (y = 0; y < FB_H_PAL; y++) {
        uint8_t *row = f->data[0] + (size_t)y * (size_t)ls_y;

        for (x = 0; x < FB_W; x++)
            row[x] = (uint8_t)(16 + ((x * 3 + y * 5) & 0xcf));
    }
    for (y = 0; y < FB_H_PAL / 2; y++) {
        uint8_t *u = f->data[1] + (size_t)y * (size_t)ls_u;
        uint8_t *v = f->data[2] + (size_t)y * (size_t)ls_v;

        for (x = 0; x < FB_W / 2; x++) {
            u[x] = (uint8_t)(80 + ((x * 7 + y) & 0x5f));
            v[x] = (uint8_t)(80 + ((y * 9 + x) & 0x5f));
        }
    }
}

static int run_perf_sws_ddr(FBPair *fb)
{
    AVFrame *src;
    struct SwsContext *sws = NULL;
    int64_t samples[SWS_DDR_ITERS];
    int i, on_screen, dest, check;
    int timed = 0, gt30 = 0, gt35 = 0, gt40 = 0, gt50 = 0;
    int64_t sum = 0, mean, med, p90, p95, p99, maxv;
    uint8_t *dst_data[4] = {0};
    int dst_linesize[4] = {0};
    uint8_t *dest_ptr;
    unsigned long dest_phys;
    const char *src_name;
    int prev_log;

    fprintf(stderr, "=== PERF ISOLATION SWS ===\n");
    fprintf(stderr,
            "sws_scale YUV420P→BGR0 into inactive O_SYNC DDR. "
            "Synthetic cached source. No mailbox, no ACK, no playback.\n");
    if (read_display_buf(fb, &on_screen) != 0) {
        fprintf(stderr,
                "FAIL: no DVD1 display_buf at 0x30400008 "
                "(cannot identify inactive framebuffer)\n");
        return 1;
    }
    dest = on_screen ^ 1;
    if (dest == on_screen) {
        fprintf(stderr, "FAIL: dest buffer is display_buf\n");
        return 1;
    }
    dest_ptr = dest ? fb->fb_b : fb->fb_a;
    dest_phys = dest ? FB_B_PHYS : FB_A_PHYS;

    src = av_frame_alloc();
    if (!src) {
        fprintf(stderr, "FAIL: av_frame_alloc\n");
        return 1;
    }
    src->format = AV_PIX_FMT_YUV420P;
    src->width = FB_W;
    src->height = FB_H_PAL;
    if (av_frame_get_buffer(src, 32) < 0) {
        fprintf(stderr, "FAIL: av_frame_get_buffer\n");
        av_frame_free(&src);
        return 1;
    }
    fill_synth_yuv420p_pal(src);

    src_name = av_get_pix_fmt_name(AV_PIX_FMT_YUV420P);
    fprintf(stderr, "source format=%s\n", src_name ? src_name : "yuv420p");
    fprintf(stderr, "source dimensions=%dx%d\n", FB_W, FB_H_PAL);
    fprintf(stderr, "destination format=bgr0\n");
    fprintf(stderr, "destination physical address=0x%08lx\n", dest_phys);
    fprintf(stderr, "SWS flags=SWS_FAST_BILINEAR (%d)\n", SWS_FAST_BILINEAR);
    fprintf(stderr,
            "display_buf=%c dest=%c stride=%d map=/dev/mem O_RDWR|O_SYNC\n",
            on_screen ? 'B' : 'A', dest ? 'B' : 'A', FB_STRIDE);

    prev_log = av_log_get_level();
    av_log_set_level(AV_LOG_INFO);
    sws = sws_getContext(FB_W, FB_H_PAL, AV_PIX_FMT_YUV420P,
                         FB_W, FB_H_PAL, AV_PIX_FMT_BGR0,
                         SWS_FAST_BILINEAR, NULL, NULL, NULL);
    av_log_set_level(prev_log);
    if (!sws) {
        fprintf(stderr, "sws_getContext failed\n");
        av_frame_free(&src);
        return 1;
    }

    if (read_display_buf(fb, &check) != 0 || check != on_screen ||
        dest == check) {
        fprintf(stderr,
                "FAIL: inactive dest could not be confirmed before convert\n");
        sws_freeContext(sws);
        av_frame_free(&src);
        return 1;
    }

    dst_data[0] = dest_ptr;
    dst_linesize[0] = FB_STRIDE;
    fprintf(stderr, "Warmup %d conversions, then %d timed iterations.\n",
            SWS_DDR_WARMUP, SWS_DDR_ITERS);
    for (i = 0; i < SWS_DDR_WARMUP && !g_interrupt; i++) {
        sws_scale(sws, (const uint8_t * const *)src->data, src->linesize,
                  0, FB_H_PAL, dst_data, dst_linesize);
    }
    for (i = 0; i < SWS_DDR_ITERS && !g_interrupt; i++) {
        int64_t t0, us;

        t0 = av_gettime_relative();
        sws_scale(sws, (const uint8_t * const *)src->data, src->linesize,
                  0, FB_H_PAL, dst_data, dst_linesize);
        us = av_gettime_relative() - t0;
        if (us < 0)
            us = 0;
        samples[timed] = us;
        sum += us;
        if (us > 30000)
            gt30++;
        if (us > 35000)
            gt35++;
        if (us > 40000)
            gt40++;
        if (us > 50000)
            gt50++;
        timed++;
    }

    fprintf(stderr, "=== PERF ISOLATION SWS ===\n");
    if (timed <= 0) {
        fprintf(stderr, "iterations=0\n");
        sws_freeContext(sws);
        av_frame_free(&src);
        return 1;
    }
    qsort(samples, (size_t)timed, sizeof(int64_t), cmp_i64);
    mean = sum / timed;
    med = sorted_pct(samples, timed, 50);
    p90 = sorted_pct(samples, timed, 90);
    p95 = sorted_pct(samples, timed, 95);
    p99 = sorted_pct(samples, timed, 99);
    maxv = samples[timed - 1];
    fprintf(stderr,
            "iterations=%d\n"
            "mean_us=%" PRId64 "\n"
            "median_us=%" PRId64 "\n"
            "p90_us=%" PRId64 "\n"
            "p95_us=%" PRId64 "\n"
            "p99_us=%" PRId64 "\n"
            "max_us=%" PRId64 "\n"
            ">30000us=%d\n"
            ">35000us=%d\n"
            ">40000us=%d\n"
            ">50000us=%d\n",
            timed, mean, med, p90, p95, p99, maxv, gt30, gt35, gt40, gt50);

    sws_freeContext(sws);
    av_frame_free(&src);
    return 0;
}

int main(int argc, char **argv)
{
    install_sigill_handler();
    install_interrupt_handler();
    setvbuf(stderr, NULL, _IONBF, 0);

    Cli cli;
    if (parse_cli(argc, argv, &cli) < 0)
        return 1;
    g_debug_stats = cli.debug_stats;
    g_debug_spu = cli.debug_spu;
    g_debug_subtitles = cli.debug_subtitles;
    g_debug_yellow_highlight = cli.debug_yellow_highlight;
    if (cli.list_titles) {
        dvdnav_t *nav = NULL;
        fprintf(stderr, "=== DVD TITLE LIST ===\nDevice: %s\n", cli.device);
        if (dvdnav_open2(&nav, NULL, &g_dvdnav_logcb, cli.device) !=
            DVDNAV_STATUS_OK)
            return 1;
        int lr = list_dvd_titles(nav);
        dvdnav_close(nav);
        return lr < 0 ? 1 : 0;
    }

    int64_t program_start = av_gettime_relative();
    fprintf(stderr, "=== SS1 THREADED DVD A/V ===\n");
    if (cli.uncapped_video_benchmark)
        fprintf(stderr, "Player mode: uncapped video benchmark\n");
    else if (cli.buffered_yuv)
        fprintf(stderr, "Player mode: buffered-YUV%s%s\n",
                cli.perf_present_no_convert ? " (ACK isolation, no per-frame sws)" : "",
                cli.phase_decode ? " (phase-decode)" : "");
    else if (cli.perf_sws_ddr)
        fprintf(stderr, "Player mode: PERF isolation SWS\n");
    else if (cli.buffered_video)
        fprintf(stderr, "Player mode: buffered-video\n");
    else
        fprintf(stderr, "Player mode: direct-DDR\n");
    if (cli.uncapped_video_benchmark)
        dbg("MODE: --uncapped-video-benchmark  "
            "(no audio, no PTS/ACK/mailbox pacing)\n");
    if (cli.buffered_video)
        dbg("MODE: --buffered-video  "
            "(cached RAM producer/consumer, MrAudio still master)\n");
    if (cli.buffered_yuv)
        fprintf(stderr, "MODE: --buffered-yuv-video  "
            "(decoded YUV queue, %s at present, "
            "MrAudio still master)\n",
            cli.fpga_yuv420 ? "FPGA YUV420 plane copy" : "direct-DDR sws");
    if (cli.fpga_yuv420 && !cli.buffered_yuv)
        fprintf(stderr, "MODE: --fpga-yuv420  "
            "(planar YUV copy, FPGA BT.601, overlays off)\n");
    if (cli.perf_present_no_convert) {
        fprintf(stderr,
                "PERF isolation ACK: per-frame sws_scale bypassed after "
                "2-frame A/B prime. PTS/stale/ACK/mailbox/decode remain.\n"
                "Picture may freeze. Timing diagnostic only. "
                "Run ~60-90s then Ctrl+C.\n");
    }
    dbg("FFmpeg CPU flags: 0x%x\n", av_get_cpu_flags());
    dbg("Queues: audio %d pkts (~1 s AC-3), video %d pkts (~1.1 s at ~340 pkt/s).\n"
        "Plays until title end, dvdnav stop, error, or Ctrl+C. Leave OSD Buffer on A.\n",
        AUDIO_Q_CAP, VIDEO_Q_CAP);
    if (g_debug_stats)
        fprintf(stderr, "Debug stats: on\n");
    if (g_debug_spu) {
        const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_DVD_SUBTITLE);
        if (dec)
            fprintf(stderr, "SPU: decoder=dvdsub\n");
        else
            fprintf(stderr,
                    "SPU: decoder=rle (dvdsub not in this FFmpeg build)\n");
        if (g_debug_yellow_highlight)
            fprintf(stderr, "SPU: yellow debug rectangle enabled\n");
    }
    if (g_debug_subtitles)
        fprintf(stderr, "Subtitle debug: on\n");

    av_log_set_level(AV_LOG_WARNING);

    FBPair fb;
    if (map_double_fb(&fb) < 0)
        return 11;

    if (cli.perf_sws_ddr) {
        int sr;

        sr = run_perf_sws_ddr(&fb);
        unmap_double_fb(&fb);
        return sr;
    }

    DVDIO d;
    memset(&d, 0, sizeof(d));
    d.last_title = -1;
    d.last_part = -1;
    d.vtsN = -1;
    if (posix_memalign((void **)&d.sector, DVD_SECTOR, DVD_SECTOR) != 0)
        return 1;

    stage = 1;
    fprintf(stderr, "DVD: %s\n", cli.device);
    if (dvdnav_open2(&d.nav, NULL, &g_dvdnav_logcb, cli.device) !=
        DVDNAV_STATUS_OK) {
        fprintf(stderr, "dvdnav_open failed\n");
        return 1;
    }
    fprintf(stderr, "libdvdnav %s\n", dvdnav_version());
    dvdnav_set_readahead_flag(d.nav, 1);
    dvdnav_menu_language_select(d.nav, "en");
    dvdnav_audio_language_select(d.nav, "en");
    dvdnav_spu_language_select(d.nav, "en");
    if (cli.authored_start) {
        fprintf(stderr, "Start: authored DVD navigation (First Play)\n");
    } else {
        if (!cli.title) {
            cli.title = 2;
            cli.chapter = 1;
        }
        fprintf(stderr, "Title %d, chapter %d\n",
                cli.title, cli.chapter ? cli.chapter : 1);
        int32_t ntitles = 0;
        dvdnav_get_number_of_titles(d.nav, &ntitles);
        dbg("Titles on disc: %d\n", (int)ntitles);
        if (jump_to_title(d.nav, &cli, ntitles) < 0)
            return 1;
    }

    const AVInputFormat *mpeg = av_find_input_format("mpeg");
    if (!mpeg)
        return 2;
    uint8_t *avio_buf = av_malloc(AVIO_BUF_SIZE);
    AVIOContext *avio = avio_alloc_context(avio_buf, AVIO_BUF_SIZE, 0, &d,
                                           dvd_read_packet, NULL, NULL);
    if (!avio)
        return 3;
    avio->seekable = 0;
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt)
        return 3;
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_GENPTS;
    fmt->ctx_flags |= AVFMTCTX_UNSEEKABLE;
    stage = 2;
    int r = avformat_open_input(&fmt, "", mpeg, NULL);
    if (r < 0) {
        char e[128];
        fferr(r, e, sizeof(e));
        fprintf(stderr, "avformat_open_input failed: %s\n", e);
        return 4;
    }
    dbg("MPEG-PS demuxer opened (demux thread owns it).\n");

    Player p;
    memset(&p, 0, sizeof(p));
    p.fb = &fb;
    p.ai = p.vi = -1;
    p.current_audio_logical = -1;
    p.audio_pending_logical = -1;
    p.audio_follow_logical = -1;
    p.audio_follow_physical = -1;
    p.audio_follow_fmt = -1;
    p.avf = fmt;
    p.last_audio_pts_us = AV_NOPTS_VALUE;
    p.uncapped_bench = cli.uncapped_video_benchmark;
    p.buffered_video = cli.buffered_video;
    p.buffered_yuv = cli.buffered_yuv;
    p.perf_present_no_convert = cli.perf_present_no_convert;
    p.phase_decode = cli.phase_decode;
    p.fpga_yuv420 = cli.fpga_yuv420;
    p.subperf.act_blend = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
    p.subperf.act_sws = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
    p.subperf.act_ack = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
    p.subperf.act_cyc = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
    p.subperf.act_sws_sub = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
    p.subperf.inact_sws = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
    p.subperf.inact_ack = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
    p.subperf.inact_cyc = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
    p.subperf.inited = p.subperf.act_blend && p.subperf.act_sws &&
                       p.subperf.act_ack && p.subperf.act_cyc &&
                       p.subperf.act_sws_sub && p.subperf.inact_sws &&
                       p.subperf.inact_ack && p.subperf.inact_cyc;
    if (p.perf_present_no_convert) {
        p.iso_ack = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
        p.iso_cyc = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
        if (!p.iso_ack || !p.iso_cyc) {
            fprintf(stderr, "FAIL: isolation sample buffers\n");
            return 1;
        }
    }
    if (p.phase_decode) {
        pthread_mutex_init(&p.phase_mu, NULL);
        pthread_cond_init(&p.phase_cv, NULL);
        p.phase_inited = 1;
        p.phase_sws = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
        p.phase_ack = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
        p.phase_cyc = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
        if (!p.phase_sws || !p.phase_ack || !p.phase_cyc) {
            fprintf(stderr, "FAIL: phase-decode sample buffers\n");
            return 1;
        }
        fprintf(stderr,
                "PHASE DECODE: producer waits during consumer sws/DDR. "
                "May refill during ACK/PTS. YUV low-water=%d  vq high-water=%d.\n",
                PHASE_YUV_LOW_WATER, PHASE_VQ_HIGH_WATER);
    }
    if (cli.fpga_yuv420) {
        p.yuv_copy = calloc(ISO_SAMPLE_CAP, sizeof(int64_t));
        if (!p.yuv_copy) {
            fprintf(stderr, "FAIL: yuv_copy sample buffer\n");
            return 1;
        }
    }
    p.first_audio_pts_us = AV_NOPTS_VALUE;
    p.initial_skip_req = cli.initial_video_skip;
    p.initial_skip_left = cli.uncapped_video_benchmark ? 0 : cli.initial_video_skip;
    p.video_advance_ms = cli.buffered_yuv ? cli.video_advance_ms : 0;
    if (cli.buffered_yuv)
        fprintf(stderr, "Buffered YUV queue: enabled (%d frames)\n",
                VIDEO_BUFFER_FRAMES);
    else if (cli.buffered_video)
        fprintf(stderr, "Buffered video queue: enabled (%d frames)\n",
                VIDEO_BUFFER_FRAMES);
    if (cli.uncapped_video_benchmark && cli.initial_video_skip)
        fprintf(stderr,
                "Initial video skip %d ignored in uncapped benchmark.\n",
                cli.initial_video_skip);
    else
        fprintf(stderr, "Initial video skip: %d\n", cli.initial_video_skip);
    if (p.video_advance_ms)
        fprintf(stderr, "Video advance: %d ms\n", p.video_advance_ms);
    else if (cli.buffered_yuv)
        fprintf(stderr, "Video advance: 0 ms\n");
    if (cli.buffered_yuv && !cli.perf_present_no_convert) {
        fprintf(stderr,
                "PRESENT PERF: every %d presents; STALE PERF on every drop.\n"
                "  phase_us = mailbox-write->ACK of previous request "
                "(not raster phase).\n",
                PRESENT_PERF_INTERVAL);
    }
    p.fpga_src_std = -1;
    p.osd_av_trim_ms = 0;
    p.fpga_v1_caps = dvd_fpga_probe_v1(&fb);
    if (p.fpga_v1_caps) {
        fprintf(stderr,
                "FPGA DVD-v1 settings live at 0x30400010 "
                "(A/V Sync + TV Auto). OSD 0 ms == this video-advance baseline.\n");
        dvd_fpga_write_source(&fb, FPGA_SRC_UNKNOWN);
        p.fpga_src_std = FPGA_SRC_UNKNOWN;
        dvd_fpga_poll_settings(&p);
    } else {
        fprintf(stderr,
                "FPGA DVD-v1 settings unavailable; A/V trim=0 ms, "
                "Auto source switching off.\n");
    }
    if (cli.fpga_yuv420) {
        if (!p.fpga_v1_caps || !dvd_fpga_yuv_cap(&fb)) {
            fprintf(stderr,
                    "FAIL: --fpga-yuv420 needs DVD_FPGA_YUV420_Test.rbf "
                    "(DVD2 YUV capability bit). Legacy BGR0 cores refused.\n");
            return 1;
        }
        fprintf(stderr,
                "FPGA YUV420 MODE: planar Y/U/V copy, mailbox bit1, "
                "FPGA BT.601. sws_scale skipped.\n"
                "FPGA YUV420 MODE: subtitles/menu overlays disabled for experiment\n");
    }
    clock_init(&p.clock);
    navq_init(&p);
    d.player = &p;
    snapshot_available_cpus(&p);
    if (g_debug_stats) {
        fprintf(stderr, "CPUs online %d / configured %d, process affinity: ",
                p.ncpu_onln, p.ncpu_conf);
        print_cpu_mask(p.cpu_aff_mask);
        fprintf(stderr, "  (all player threads unpinned)\n");
    }
    if (pktq_init(&p.aq, AUDIO_Q_CAP) < 0 || pktq_init(&p.vq, VIDEO_Q_CAP) < 0)
        return 6;
    if (player_buffered(&p)) {
        p.prefill_req = VIDEO_PREFILL_FRAMES;
        p.prefill_t0_us = av_gettime_relative();
        pthread_mutex_init(&p.prefill_mu, NULL);
        pthread_cond_init(&p.prefill_cv, NULL);
        if (p.buffered_yuv) {
            if (yuvring_init(&p.yuvring) < 0)
                return 6;
        } else if (vidring_init(&p.vring) < 0) {
            return 6;
        }
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return 6;

    pthread_t ath, vth, pth, ith;
    int last_diag_s = -1;
    int last_play_rendered = 0;
    int64_t last_play_us = 0;
    int64_t demux_t0 = av_gettime_relative();
    int read_ret = 0;
    const char *stop_reason = "unknown";

    if (pthread_create(&ith, NULL, input_thread, &p) != 0) {
        fprintf(stderr,
                "WARNING: controller input thread failed to start "
                "(playback continues).\n");
    } else {
        p.input_started = 1;
    }

    dbg("\nDemuxing (continuous, backpressure on full queues)...\n");

    for (;;) {
        if (p.fail || g_interrupt)
            break;
        dvdio_process_nav_cmds(&d);
        if (d.stopped)
            break;
        if (pause_is_held(&p) && !d.hop_pending && !p.demux_reopen_req) {
            pause_wait_control(&p);
            continue;
        }
        read_ret = av_read_frame(fmt, pkt);
        if (read_ret < 0) {
            if (!p.fail && !g_interrupt && !d.stopped &&
                (d.hop_pending || p.demux_reopen_req)) {
                d.hop_pending = 0;
                p.demux_reopen_req = 0;
                if (fmt->pb) {
                    fmt->pb->eof_reached = 0;
                    fmt->pb->error = 0;
                    /* Discard pre-hop MPEG-PS still sitting in the AVIO
                     * buffer so the title start is not mixed with menu packs. */
                    fmt->pb->buf_ptr = fmt->pb->buf_end;
                    fmt->pb->buf_ptr_max = fmt->pb->buf_end;
                }
                avformat_flush(fmt);
                avcodec_parameters_free(&p.vcp);
                avcodec_parameters_free(&p.acp);
                p.vi = -1;
                p.ai = -1;
                dbg("DVD MENU: MPEG-PS demux flushed/reopened for "
                    "domain change  gen=%u\n",
                    player_nav_gen(&p));
                continue;
            }
            break;
        }
        if (p.vi < 0 || p.ai < 0) {
            for (unsigned i = 0; i < fmt->nb_streams; ++i) {
                AVCodecParameters *cp = fmt->streams[i]->codecpar;
                if (p.vi < 0 && cp->codec_type == AVMEDIA_TYPE_VIDEO) {
                    p.vi = (int)i;
                    p.vtb = fmt->streams[i]->time_base;
                    p.avg_fr = fmt->streams[i]->avg_frame_rate;
                    p.r_fr = fmt->streams[i]->r_frame_rate;
                    if (copy_codecpar(&p.vcp, cp) < 0) {
                        player_abort(&p);
                        break;
                    }
                    if (p.vcp->codec_id == AV_CODEC_ID_NONE)
                        p.vcp->codec_id = AV_CODEC_ID_MPEG2VIDEO;
                    dbg("Video stream #%d %s tb %d/%d\n",
                        p.vi, avcodec_get_name(p.vcp->codec_id),
                        p.vtb.num, p.vtb.den);
                    if (p.video_started)
                        dbg("DVD MENU: video stream parameters refreshed "
                            "(decoder flushed on gen, not reopened)\n");
                }
                if (p.ai < 0 && cp->codec_type == AVMEDIA_TYPE_AUDIO) {
                    p.ai = (int)i;
                    p.atb = fmt->streams[i]->time_base;
                    if (copy_codecpar(&p.acp, cp) < 0) {
                        player_abort(&p);
                        break;
                    }
                    dbg("Audio stream #%d %s %d Hz %d ch tb %d/%d\n",
                        p.ai, avcodec_get_name(p.acp->codec_id),
                        p.acp->sample_rate, p.acp->ch_layout.nb_channels,
                        p.atb.num, p.atb.den);
                    if (p.audio_started)
                        dbg("DVD MENU: audio stream parameters refreshed "
                            "(decoder flushed on gen, not reopened)\n");
                }
            }
        }
        player_apply_audio_follow(&p);

        if (p.in_menu)
            menu_spu_log_streams(&p, fmt);

        if (p.ai >= 0 && p.acp && !p.audio_started && !p.uncapped_bench) {
            if (pthread_create(&ath, NULL, audio_thread, &p) != 0) {
                fprintf(stderr, "pthread_create(audio) failed\n");
                player_abort(&p);
                break;
            }
            p.audio_started = 1;
            dbg("Audio worker started.\n");
        }
        if (p.vi >= 0 && p.vcp && !p.video_started) {
            if (pthread_create(&vth, NULL, video_thread, &p) != 0) {
                fprintf(stderr, "pthread_create(video) failed\n");
                player_abort(&p);
                break;
            }
            p.video_started = 1;
            dbg("Video worker started.\n");
            if (player_buffered(&p) && !p.present_started) {
                if (pthread_create(&pth, NULL, present_thread, &p) != 0) {
                    fprintf(stderr, "pthread_create(present) failed\n");
                    player_abort(&p);
                    break;
                }
                p.present_started = 1;
                dbg("Presentation consumer started.\n");
            }
        }
        if (p.uncapped_bench) {
            if (p.video_started && p.sched_demux_cpu < 0)
                note_unpinned_cpu("demux", &p.sched_demux_cpu);
        } else if (p.audio_started && p.video_started && p.sched_demux_cpu < 0) {
            note_unpinned_cpu("demux", &p.sched_demux_cpu);
        }

        if (p.uncapped_bench && p.ai >= 0 && pkt->stream_index == p.ai) {
            p.bench_audio_discarded++;
        } else if (p.audio_started && pkt->stream_index == p.ai) {
            player_pktq_push(&p, &p.aq, pkt);
        } else if (p.video_started && pkt->stream_index == p.vi) {
            if (p.soft_log_pkt &&
                player_nav_gen(&p) == p.soft_nav_gen) {
                p.soft_log_pkt = 0;
                dbg("DVD MENU: first MPEG video packet after SOFT HOP  "
                    "gen=%u size=%d\n",
                    player_nav_gen(&p), pkt->size);
            }
            player_pktq_push(&p, &p.vq, pkt);
        } else if (p.in_menu && pkt->data && pkt->size > 0) {
            menu_spu_note_decoder(&p);
            menu_spu_log_streams(&p, fmt);
            if (menu_spu_packet_wanted(&p, fmt, pkt)) {
                if (p.spu.pes_id < 0)
                    p.spu.pes_id = fmt->streams[pkt->stream_index]->id & 0xff;
                menu_spu_feed_packet(&p, pkt->data, pkt->size,
                                     player_nav_gen(&p));
            }
        } else if (!p.in_menu && pkt->data && pkt->size > 0) {
            if (p.msub.pes_id < 0 && d.nav) {
                int8_t active = dvdnav_get_active_spu_stream(d.nav);

                if (active >= 0)
                    movie_sub_set_stream(&p, p.msub.logical < 0 ? 0
                                                               : p.msub.logical,
                                         active & 0x1f, -1, -1, (int)active);
            }
            if (movie_sub_packet_wanted(&p, fmt, pkt)) {
                AVStream *sst = fmt->streams[pkt->stream_index];
                int64_t spts = AV_NOPTS_VALUE;

                if (pkt->pts != AV_NOPTS_VALUE && sst)
                    spts = tb_to_us(sst->time_base, pkt->pts);
                movie_sub_feed_packet(&p, pkt->data, pkt->size, spts);
            }
        }

        av_packet_unref(pkt);

        {
            int64_t elapsed = 0, aclk = 0;
            int fill = 0;
            pthread_mutex_lock(&p.clock.mu);
            elapsed = p.clock.elapsed_us;
            aclk = p.clock.clock_us;
            fill = p.clock.fill;
            pthread_mutex_unlock(&p.clock.mu);
            int sec = (int)(elapsed / 1000000);
            int wall_s = (int)((av_gettime_relative() - program_start) / 1000000);
            if (p.uncapped_bench) {
                if (wall_s != last_diag_s && (wall_s % 10) == 0 && wall_s > 0) {
                    last_diag_s = wall_s;
                    double bdur = 0.0;
                    if (p.bench_t0_us)
                        bdur = (av_gettime_relative() - p.bench_t0_us) / 1e6;
                    fprintf(stderr,
                            "PLAY  t=%ds  fps=%.2f  buf=%d/%d  stale=0  "
                            "underrun=0  av=n/a\n",
                            wall_s,
                            (bdur > 0.0) ? p.bench_converted / bdur : 0.0,
                            pktq_count(&p.vq), VIDEO_Q_CAP);
                    dbg("  wall=%ds  uncapped converted=%d  "
                        "decode+DDR fps=%.2f  qV=%d  src_fps=%.3f\n",
                        wall_s, p.bench_converted,
                        (bdur > 0.0) ? p.bench_converted / bdur : 0.0,
                        pktq_count(&p.vq),
                        (p.fps.num > 0 && p.fps.den > 0)
                            ? av_q2d(p.fps) : 0.0);
                }
            } else if (sec != last_diag_s && (sec % 10) == 0 && sec > 0) {
                int64_t now_us = av_gettime_relative();
                int64_t dt_us = last_play_us
                                ? (now_us - last_play_us)
                                : (now_us - program_start);
                int dframes = p.rendered - last_play_rendered;
                double ifps = (dt_us > 0) ? dframes / (dt_us / 1e6) : 0.0;
                int buf_n = player_buffered(&p)
                            ? buffered_queue_count(&p)
                            : pktq_count(&p.vq);
                int buf_cap = player_buffered(&p)
                              ? VIDEO_BUFFER_FRAMES
                              : VIDEO_Q_CAP;

                last_diag_s = sec;
                last_play_us = now_us;
                last_play_rendered = p.rendered;
                fprintf(stderr,
                        "PLAY  t=%ds  fps=%.2f  buf=%d/%d  stale=%d  "
                        "underrun=%d  av=%.0fms\n",
                        sec, ifps, buf_n, buf_cap, p.stale_dropped,
                        p.live_underruns, p.last_offset / 1000.0);
                if (player_buffered(&p))
                    dbg("  wall=%ds  consumed=%ds  aclk=%.3fs  vpts=%.3fs"
                        "  v-a_raw=%+.1fms v-a_pres=%+.1fms  fill=%d (%.0fms)"
                        "  qA=%d qV=%d buf=%d"
                        "  frames=%d  stale=%d  late=%d  ack_iv=%.1fms\n",
                        wall_s, sec,
                        aclk == AV_NOPTS_VALUE ? 0.0 : aclk / 1e6,
                        p.last_video_pts_us == AV_NOPTS_VALUE
                            ? 0.0 : p.last_video_pts_us / 1e6,
                        p.last_raw_offset / 1000.0, p.last_offset / 1000.0,
                        fill, fill * 1000.0 / (double)BYTES_PER_SEC,
                        pktq_count(&p.aq), pktq_count(&p.vq),
                        buffered_queue_count(&p),
                        p.rendered, p.stale_dropped, p.frames_late,
                        p.last_ack_interval_us / 1000.0);
                else
                    dbg("  wall=%ds  consumed=%ds  aclk=%.3fs  vpts=%.3fs"
                        "  v-a_raw=%+.1fms v-a_pres=%+.1fms  fill=%d (%.0fms)"
                        "  qA=%d qV=%d"
                        "  frames=%d  stale=%d  late=%d  ack_iv=%.1fms\n",
                        wall_s, sec,
                        aclk == AV_NOPTS_VALUE ? 0.0 : aclk / 1e6,
                        p.last_video_pts_us == AV_NOPTS_VALUE
                            ? 0.0 : p.last_video_pts_us / 1e6,
                        p.last_raw_offset / 1000.0, p.last_offset / 1000.0,
                        fill, fill * 1000.0 / (double)BYTES_PER_SEC,
                        pktq_count(&p.aq), pktq_count(&p.vq),
                        p.rendered, p.stale_dropped, p.frames_late,
                        p.last_ack_interval_us / 1000.0);
            }
        }
    }

    if (g_interrupt) {
        stop_reason = "Ctrl+C / SIGTERM";
        player_abort(&p);
    } else if (p.fail) {
        stop_reason = "fatal error";
    } else if (d.stopped) {
        stop_reason = "dvdnav STOP / title end";
    } else if (read_ret == AVERROR_EOF) {
        stop_reason = "demux EOF";
    } else if (read_ret < 0) {
        stop_reason = "demux error";
    } else {
        stop_reason = "demux loop ended";
    }

    p.demux_cpu_us = av_gettime_relative() - demux_t0;
    pktq_eof(&p.aq);
    pktq_eof(&p.vq);
    if (p.buffered_video)
        vidring_eof(&p.vring);
    if (p.buffered_yuv)
        yuvring_eof(&p.yuvring);
    if (p.audio_started)
        pthread_join(ath, NULL);
    if (p.video_started)
        pthread_join(vth, NULL);
    if (p.present_started)
        pthread_join(pth, NULL);
    if (p.input_started)
        pthread_join(ith, NULL);

    int64_t wall_us = av_gettime_relative() - program_start;
    double wall = wall_us / 1e6;
    const MrAudio *mr = &p.mr_stats;
    double hw_dur = (double)mraudio_consumed(mr) / (double)BYTES_PER_SEC;
    double fill_avg = mr->fill_n ? (double)mr->fill_sum / mr->fill_n : 0.0;
    double aq_avg = p.aq.depth_n ? (double)p.aq.depth_sum / p.aq.depth_n : 0.0;
    double vq_avg = p.vq.depth_n ? (double)p.vq.depth_sum / p.vq.depth_n : 0.0;
    double off_avg = p.offset_n ? (double)p.offset_sum / p.offset_n : 0.0;
    double raw_off_avg = p.raw_offset_n
                         ? (double)p.raw_offset_sum / p.raw_offset_n : 0.0;
    double convert_avg_ms = p.rendered
                            ? (p.convert_us / 1000.0) / p.rendered : 0.0;
    int timed_decoded = p.video_decoded;
    int accounted = p.rendered + p.stale_dropped;

    if (p.uncapped_bench) {
        double bench_dur = 0.0;
        if (p.bench_t0_us && p.bench_t1_us >= p.bench_t0_us)
            bench_dur = (p.bench_t1_us - p.bench_t0_us) / 1e6;
        double src_fps = (p.fps.num > 0 && p.fps.den > 0) ? av_q2d(p.fps) : 0.0;
        double uncap_fps = (bench_dur > 0.0) ? p.bench_converted / bench_dur : 0.0;
        double dec_avg = p.bench_dec_n
                         ? (double)p.bench_dec_sum / p.bench_dec_n : 0.0;
        double sws_avg = p.bench_sws_n
                         ? (double)p.bench_sws_sum / p.bench_sws_n : 0.0;
        double sws_cpu_avg = p.bench_sws_cpu_n
                             ? (double)p.bench_sws_cpu_sum / p.bench_sws_cpu_n
                             : 0.0;
        double combo_avg = p.bench_combo_n
                           ? (double)p.bench_combo_sum / p.bench_combo_n : 0.0;
        int mbox_unchanged = (p.bench_mbox0_start == p.bench_mbox0_end);
        int scanout_unchanged = (p.bench_bufs_ok &&
                                 p.bench_display_end == p.bench_active_buf);
        int64_t total_cpu = 0;
        if (p.bench_video_cpu_us > 0)
            total_cpu += p.bench_video_cpu_us;

        fprintf(stderr,
                "\n=== DVD PLAYBACK SUMMARY ===\n"
                "Duration:                   %.3f s\n"
                "Audio consumed:             0.000 s\n"
                "Video decoded:              %d\n"
                "Video presented:            %d\n"
                "Presented fps:              %.3f\n"
                "Stale drops:                0\n"
                "A/V offset average:         n/a\n"
                "A/V offset start/end:       n/a\n"
                "Audio underruns:            0\n"
                "PTS breaks:                 0 / %d\n"
                "Video buffer min/avg/max:   n/a\n"
                "Missed native boundaries:   0\n"
                "Stop reason:                %s\n",
                wall, p.bench_decoded, p.bench_converted, uncap_fps,
                p.video_disc, stop_reason);

        if (g_debug_stats) {
        fprintf(stderr,
                "\n=== UNCAPPED VIDEO THROUGHPUT REPORT ===\n"
                "Stop reason:                %s\n"
                "Wall benchmark duration:    %8.3f s  "
                "(first convert start → last convert end)\n"
                "Program wall duration:      %8.3f s\n"
                "Timed frames decoded:       %d\n"
                "Timed frames converted DDR: %d\n"
                "A. Decode/frame production min/avg/max: %.2f / %.2f / %.2f ms"
                "  n=%lu\n"
                "   (interval after previous convert → this receive_frame;\n"
                "    first frame excluded)\n"
                "B. sws_scale to DDR wall min/avg/max:   %.2f / %.2f / %.2f ms"
                "  n=%lu\n"
                "   sws_scale CPU min/avg/max:           %.2f / %.2f / %.2f ms"
                "  n=%lu\n"
                "C. Decode+convert per-frame min/avg/max: %.2f / %.2f / %.2f ms"
                "  n=%lu\n"
                "C. Complete decode+convert throughput:  %.3f fps\n"
                "Source fps:                 %.3f  (%d/%d)\n"
                "Ratio vs source fps:        %.3f x\n"
                "Video thread wall:          %.3f s\n"
                "Video thread CPU:           %.3f s\n"
                "Demux thread wall:          %.3f s\n"
                "Total video-thread CPU:     %.3f s\n"
                "Decode errors:              %d\n"
                "Video PTS breaks:           %d\n"
                "Audio packets discarded:    %lu  (not decoded, no MrAudio)\n"
                "Mailbox writes:             %lu\n"
                "Mailbox word start → end:   0x%08" PRIx32 " → 0x%08" PRIx32
                "  %s\n"
                "Active scanout buffer:      %s  (unchanged=%s)\n"
                "Inactive benchmark target:  %s  phys 0x%08lx\n"
                "Final display_buf:          %s\n"
                "Video queue full/wait:      %lu times, %.3f s\n"
                "Pacing:                     none (uncapped benchmark)\n",
                stop_reason,
                bench_dur, wall,
                p.bench_decoded, p.bench_converted,
                p.bench_dec_min / 1000.0, dec_avg / 1000.0,
                p.bench_dec_max / 1000.0, p.bench_dec_n,
                p.bench_sws_min / 1000.0, sws_avg / 1000.0,
                p.bench_sws_max / 1000.0, p.bench_sws_n,
                p.bench_sws_cpu_min / 1000.0, sws_cpu_avg / 1000.0,
                p.bench_sws_cpu_max / 1000.0, p.bench_sws_cpu_n,
                p.bench_combo_min / 1000.0, combo_avg / 1000.0,
                p.bench_combo_max / 1000.0, p.bench_combo_n,
                uncap_fps, src_fps, p.fps.num, p.fps.den,
                (src_fps > 0.0) ? uncap_fps / src_fps : 0.0,
                p.video_cpu_us / 1e6,
                p.bench_video_cpu_us / 1e6,
                p.demux_cpu_us / 1e6,
                total_cpu / 1e6,
                p.bench_decode_errors, p.video_disc,
                p.bench_audio_discarded,
                p.mailbox_writes,
                p.bench_mbox0_start, p.bench_mbox0_end,
                mbox_unchanged ? "(unchanged — no mailbox writes)"
                               : "(CHANGED — unexpected)",
                p.bench_bufs_ok ? fb_letter(p.bench_active_buf) : "?",
                scanout_unchanged ? "yes" : "NO",
                p.bench_bufs_ok ? fb_letter(p.bench_target_buf) : "?",
                p.bench_bufs_ok
                    ? (p.bench_target_buf ? FB_B_PHYS : FB_A_PHYS) : 0UL,
                p.bench_display_end >= 0
                    ? fb_letter(p.bench_display_end) : "unreadable",
                p.vq.full_n, p.vq.block_us / 1e6);
        fprintf(stderr, "\n=== THREAD SCHEDULING ===\n");
        fprintf(stderr,
                "Available CPUs:             %d online / %d configured  [",
                p.ncpu_onln, p.ncpu_conf);
        print_cpu_mask(p.cpu_aff_mask);
        fprintf(stderr, "]\n");
        fprintf(stderr,
                "Video affinity:             unpinned  sched_getcpu=%d\n"
                "Audio affinity:             not started (benchmark)\n"
                "Input affinity:             unpinned  sched_getcpu=%d\n"
                "Demux affinity:             unpinned  sched_getcpu=%d\n",
                p.sched_video_cpu, p.sched_input_cpu, p.sched_demux_cpu);
        }
    }

    if (!p.uncapped_bench) {
        int buf_min = 0, buf_max = 0;
        double buf_avg = 0.0;
        int buf_valid = 0;
        if (p.rendered > 0 && p.buffered_yuv && p.yuvring.depth_n) {
            buf_min = p.yuvring.depth_min;
            buf_max = p.yuvring.depth_max;
            buf_avg = (double)p.yuvring.depth_sum / p.yuvring.depth_n;
            buf_valid = 1;
        } else if (p.rendered > 0 && p.buffered_video && p.vring.depth_n) {
            buf_min = p.vring.depth_min;
            buf_max = p.vring.depth_max;
            buf_avg = (double)p.vring.depth_sum / p.vring.depth_n;
            buf_valid = 1;
        }
        fprintf(stderr,
                "\n=== DVD PLAYBACK SUMMARY ===\n"
                "DVD video standard:         %s",
                p.dvd_std != DVD_VIDEO_UNKNOWN
                    ? dvd_std_name(p.dvd_std) : "n/a");
        if (p.dvd_std != DVD_VIDEO_UNKNOWN)
            fprintf(stderr, " %dx%d @ %.3f fps\n",
                    p.video_w, p.video_h,
                    (p.fps.num > 0 && p.fps.den > 0) ? av_q2d(p.fps) : 0.0);
        else
            fprintf(stderr, "\n");
        fprintf(stderr,
                "Duration:                   %.3f s\n"
                "Audio consumed:             %.3f s\n"
                "Video decoded:              %d\n"
                "Video presented:            %d\n"
                "Presented fps:              %.3f\n"
                "Stale drops:                %d\n"
                "A/V offset average:         %+.2f ms\n"
                "A/V offset start/end:       %+.1f / %+.1f ms\n"
                "Audio underruns:            %d\n"
                "PTS breaks:                 %d / %d\n",
                wall, hw_dur, p.video_decoded, p.rendered,
                (hw_dur > 0.0) ? p.rendered / hw_dur : 0.0,
                p.stale_dropped, off_avg / 1000.0,
                (p.first_off_n ? p.first_offsets[0] : 0) / 1000.0,
                p.last_offset / 1000.0,
                mr->underruns, p.audio_disc, p.video_disc);
        if (buf_valid)
            fprintf(stderr,
                    "Video buffer min/avg/max:   %d / %.1f / %d\n",
                    buf_min, buf_avg, buf_max);
        else
            fprintf(stderr,
                    "Video buffer min/avg/max:   n/a (playback did not start)\n");
        fprintf(stderr,
                "Missed native boundaries:   %u\n"
                "Stop reason:                %s\n",
                p.miss_total, stop_reason);
        if (g_debug_spu || p.spu_perf.menu_frames) {
            unsigned long n = p.spu_perf.menu_frames;
            unsigned long sr = p.spu_perf.still_redraws;
            double avg_sws = n ? (double)p.spu_perf.sws_sum / (double)n : 0.0;
            double avg_ov = n ? (double)p.spu_perf.ov_sum / (double)n : 0.0;
            double avg_still = sr ? (double)p.spu_perf.still_sum / (double)sr
                                  : 0.0;
            double avg_px = n ? (double)p.spu_perf.px_sum / (double)n : 0.0;
            double avg_bbox = n ? (double)p.spu_perf.bbox_sum / (double)n : 0.0;

            fprintf(stderr,
                    "SPU PERF:\n"
                    "  menu frames=%lu\n"
                    "  avg sws_us=%.1f\n"
                    "  avg overlay_us=%.1f\n"
                    "  max overlay_us=%" PRId64 "\n"
                    "  still redraws=%lu\n"
                    "  avg still_redraw_us=%.1f\n"
                    "  overlay pixels/frame avg=%.1f\n"
                    "  overlay bbox avg=%.1f max=%u\n",
                    n, avg_sws, avg_ov, p.spu_perf.ov_max,
                    sr, avg_still, avg_px, avg_bbox, p.spu_perf.bbox_max);
        }
        if (g_debug_subtitles || p.msub.complete_spus || p.msub.displayed_spus ||
            p.msub.spu_fragments) {
            fprintf(stderr,
                    "SUBTITLE STATS:\n"
                    "  spu_fragments=%lu\n"
                    "  complete_spus=%lu\n"
                    "  decoded_spus=%lu\n"
                    "  displayed_spus=%lu\n"
                    "  malformed_spus=%lu\n"
                    "  chg_colcon_spus=%lu\n"
                    "  chg_colcon_events=%lu\n"
                    "  malformed_chg_colcon=%lu\n",
                    p.msub.spu_fragments, p.msub.complete_spus,
                    p.msub.decoded_spus, p.msub.displayed_spus,
                    p.msub.malformed_spus, p.msub.chg_colcon_spus,
                    p.msub.chg_colcon_events, p.msub.malformed_chg_colcon);
            if (p.msub.bbox_n)
                fprintf(stderr,
                        "  bbox avg=%ux%u max=%ux%u n=%u\n",
                        (unsigned)(p.msub.bbox_w_sum / p.msub.bbox_n),
                        (unsigned)(p.msub.bbox_h_sum / p.msub.bbox_n),
                        p.msub.bbox_w_max, p.msub.bbox_h_max, p.msub.bbox_n);
        }
        if (p.subperf.act_frames || p.subperf.inact_frames) {
            fprintf(stderr, "SUBTITLE BLEND PERF:\n");
            fprintf(stderr, "  ACTIVE frames=%lu stale=%lu missed_boundaries=%lu\n",
                    p.subperf.act_frames, p.subperf.act_stale,
                    p.subperf.act_miss);
            subperf_print_dist("blend_us", p.subperf.act_blend,
                               p.subperf.act_blend_n, p.subperf.act_blend_sum,
                               p.subperf.act_blend_max);
            subperf_print_dist("sws_us", p.subperf.act_sws,
                               p.subperf.act_sws_n, p.subperf.act_sws_sum, 0);
            subperf_print_dist("sws+blend_us", p.subperf.act_sws_sub,
                               p.subperf.act_combo_n, p.subperf.act_combo_sum,
                               0);
            subperf_print_dist("ack_us", p.subperf.act_ack,
                               p.subperf.act_ack_n, p.subperf.act_ack_sum, 0);
            subperf_print_dist("cycle_us", p.subperf.act_cyc,
                               p.subperf.act_cyc_n, p.subperf.act_cyc_sum, 0);
            fprintf(stderr,
                    "  INACTIVE frames=%lu stale=%lu missed_boundaries=%lu\n",
                    p.subperf.inact_frames, p.subperf.inact_stale,
                    p.subperf.inact_miss);
            subperf_print_dist("sws_us", p.subperf.inact_sws,
                               p.subperf.inact_sws_n, p.subperf.inact_sws_sum,
                               0);
            subperf_print_dist("ack_us", p.subperf.inact_ack,
                               p.subperf.inact_ack_n, p.subperf.inact_ack_sum,
                               0);
            subperf_print_dist("cycle_us", p.subperf.inact_cyc,
                               p.subperf.inact_cyc_n, p.subperf.inact_cyc_sum,
                               0);
        }

    if (g_debug_stats) {
    fprintf(stderr,
            "\n=== THREADED A/V REPORT ===\n"
            "Video pipeline:             %s\n"
            "Stop reason:                %s\n"
            "Wall playback duration:     %8.3f s\n"
            "Hardware audio consumed:    %8.3f s\n"
            "Audio samples written:      %" PRId64 "  (%.3f s)\n"
            "Video frames decoded:       %d  (timed)  preroll %d\n"
            "Video frames presented:     %d  (A %d / B %d)\n"
            "Initial video skip requested: %d\n"
            "Initial video frames skipped: %d\n"
            "Initial frame advance:      %d frame%s / %.3f ms\n"
            "Additional video advance:   %.3f ms  (--video-advance-ms)\n"
            "Total presentation phase:   %.3f ms  (N*T + video-advance)\n"
            "Video frames stale-dropped: %d\n"
            "Presented fps:              %.3f  (source %.3f)\n"
            "Decoded timeline fps:       %.3f  (presented+stale / audio)\n"
            "Audio packets/frames:       %lu / %lu\n"
            "Video packets queued:       %lu\n"
            "Audio queue depth min/avg/max: %d / %.1f / %d\n"
            "Video queue depth min/avg/max: %d / %.1f / %d\n"
            "Audio queue full:           %lu times, demux wait %.3f s\n"
            "Video queue full:           %lu times, demux wait %.3f s\n"
            "Demux backpressure wait:    %.3f s  (A %.3f + V %.3f)\n"
            "MrAudio fill min/avg/max:   %" PRId64 " / %.0f / %" PRId64
            " bytes (%.1f / %.1f / %.1f ms)\n"
            "MrAudio underruns:          %d  (%.3f s)\n"
            "A/V offset samples:         %d\n"
            "Presentation PTS lead:      %.3f ms\n"
            "Average raw mailbox v-a:    %+.2f ms  (raw_vpts-aclk)\n"
            "Average presentation v-a:   %+.2f ms  (adjusted presentation_vpts-aclk)\n"
            "Max presentation ahead/behind: %+.2f / %+.2f ms\n"
            "Max raw ahead/behind:       %+.2f / %+.2f ms\n"
            "Frames >40 ms / >80 ms late:%d / %d\n"
            "Audio/video PTS breaks:     %d / %d\n"
            "Average direct-DDR sws:     %.2f ms/frame\n"
            "Average RAM→DDR memcpy:     %.2f ms/frame\n"
            "Thread CPU (wall in thread): demux %.3fs  audio %.3fs  "
            "video %.3fs  present %.3fs  input %.3fs\n"
            "Pacing:                     %s\n",
            p.buffered_yuv
                ? "BUFFERED YUV queue / direct-DDR sws consumer"
                : p.buffered_video
                    ? "BUFFERED CACHED-RAM producer/consumer"
                    : "direct-DDR decode+sws",
            stop_reason,
            wall, hw_dur, p.out_samples,
            (double)p.out_samples / (double)OUT_RATE,
            p.video_decoded, p.preroll_decoded,
            p.rendered, p.frames_a, p.frames_b,
            p.initial_skip_req, p.initial_video_skipped,
            p.initial_skip_req, p.initial_skip_req == 1 ? "" : "s",
            presentation_phase_us(&p) / 1000.0,
            video_advance_applied_us(&p) / 1000.0,
            total_presentation_phase_us(&p) / 1000.0,
            p.stale_dropped,
            (hw_dur > 0.0) ? p.rendered / hw_dur : 0.0,
            (p.fps.num > 0 && p.fps.den > 0) ? av_q2d(p.fps) : 0.0,
            (hw_dur > 0.0) ? accounted / hw_dur : 0.0,
            p.audio_packets, p.audio_frames,
            p.vq.pushed,
            p.aq.depth_min == AUDIO_Q_CAP && !p.aq.depth_n ? 0 : p.aq.depth_min,
            aq_avg, p.aq.depth_max,
            p.vq.depth_min == VIDEO_Q_CAP && !p.vq.depth_n ? 0 : p.vq.depth_min,
            vq_avg, p.vq.depth_max,
            p.aq.full_n, p.aq.block_us / 1e6,
            p.vq.full_n, p.vq.block_us / 1e6,
            (p.aq.block_us + p.vq.block_us) / 1e6,
            p.aq.block_us / 1e6, p.vq.block_us / 1e6,
            mr->fill_min, fill_avg, mr->fill_max,
            mr->fill_min * 1000.0 / BYTES_PER_SEC,
            fill_avg * 1000.0 / BYTES_PER_SEC,
            mr->fill_max * 1000.0 / BYTES_PER_SEC,
            mr->underruns, mr->underrun_us / 1e6,
            p.offset_n,
            EARLY_SLACK_US / 1000.0,
            raw_off_avg / 1000.0,
            off_avg / 1000.0,
            (double)p.offset_max_pos / 1000.0,
            (double)p.offset_max_neg / 1000.0,
            (double)p.raw_offset_max_pos / 1000.0,
            (double)p.raw_offset_max_neg / 1000.0,
            p.late_40, p.late_80,
            p.audio_disc, p.video_disc,
            p.buffered_video ? 0.0 : convert_avg_ms,
            p.buffered_video ? convert_avg_ms : 0.0,
            p.demux_cpu_us / 1e6, p.audio_cpu_us / 1e6, p.video_cpu_us / 1e6,
            p.present_cpu_us / 1e6, p.input_cpu_us / 1e6,
            mr->hw_pace ? "hardware FPGA rptr/len" : "DEGRADED wall-clock");

    fprintf(stderr,
            "\n=== DVD PAUSE ===\n"
            "Pause events:               %u\n"
            "Last request fill:          %d bytes  (%.1f ms)\n"
            "Consumed during drain:      %" PRId64 " bytes  (%.1f ms)\n"
            "Drain duration:             %.1f ms\n"
            "A/V offset at request:      %s%+.2f ms\n"
            "A/V offset at held:         %s%+.2f ms\n"
            "MrAudio fill at held:       %d bytes  (%.3f ms)\n"
            "Clock at request:           %s%.6f s\n"
            "Clock at held:              %s%.6f s\n"
            "Clock at resume:            %s%.6f s\n"
            "First video PTS after resume: %s%.6f s\n"
            "First A/V offset after resume: %s%+.2f ms\n"
            "First audio write after resume: %s%.1f ms after resume\n"
            "Stale at request / resume / end: %d / %d / %d  "
            "(after resume %d)\n"
            "Queues at held aq/vq/yuv:   %d / %d / %d\n",
            p.pause.events,
            p.pause.fill_at_req,
            p.pause.fill_at_req * 1000.0 / (double)BYTES_PER_SEC,
            p.pause.consumed_at_held - p.pause.consumed_at_req,
            (p.pause.consumed_at_held - p.pause.consumed_at_req) *
                1000.0 / (double)BYTES_PER_SEC,
            p.pause.drain_us / 1000.0,
            p.pause.av_off_at_req == AV_NOPTS_VALUE ? "(none) " : "",
            p.pause.av_off_at_req == AV_NOPTS_VALUE ? 0.0
                : p.pause.av_off_at_req / 1000.0,
            p.pause.av_off_at_held == AV_NOPTS_VALUE ? "(none) " : "",
            p.pause.av_off_at_held == AV_NOPTS_VALUE ? 0.0
                : p.pause.av_off_at_held / 1000.0,
            p.pause.fill_at_held,
            p.pause.fill_at_held * 1000.0 / (double)BYTES_PER_SEC,
            p.pause.clock_at_req == AV_NOPTS_VALUE ? "(none) " : "",
            p.pause.clock_at_req == AV_NOPTS_VALUE ? 0.0
                : p.pause.clock_at_req / 1e6,
            p.pause.clock_at_held == AV_NOPTS_VALUE ? "(none) " : "",
            p.pause.clock_at_held == AV_NOPTS_VALUE ? 0.0
                : p.pause.clock_at_held / 1e6,
            p.pause.clock_at_resume == AV_NOPTS_VALUE ? "(none) " : "",
            p.pause.clock_at_resume == AV_NOPTS_VALUE ? 0.0
                : p.pause.clock_at_resume / 1e6,
            p.pause.first_vpts_resume == AV_NOPTS_VALUE ? "(none) " : "",
            p.pause.first_vpts_resume == AV_NOPTS_VALUE ? 0.0
                : p.pause.first_vpts_resume / 1e6,
            p.pause.av_off_at_resume == AV_NOPTS_VALUE ? "(none) " : "",
            p.pause.av_off_at_resume == AV_NOPTS_VALUE ? 0.0
                : p.pause.av_off_at_resume / 1000.0,
            (p.pause.first_audio_write_resume == AV_NOPTS_VALUE ||
             p.pause.resume_us == AV_NOPTS_VALUE ||
             p.pause.resume_us == 0) ? "(none) " : "",
            (p.pause.first_audio_write_resume == AV_NOPTS_VALUE ||
             p.pause.resume_us == AV_NOPTS_VALUE ||
             p.pause.resume_us == 0)
                ? 0.0
                : (p.pause.first_audio_write_resume - p.pause.resume_us) /
                  1000.0,
            p.pause.stale_at_req, p.pause.stale_at_resume, p.stale_dropped,
            p.stale_dropped - p.pause.stale_at_resume,
            p.pause.aq_at_held, p.pause.vq_at_held, p.pause.yuv_at_held);

    {
        fprintf(stderr, "\n=== THREAD SCHEDULING ===\n");
        fprintf(stderr, "Available CPUs:             %d online / %d configured  [",
                p.ncpu_onln, p.ncpu_conf);
        print_cpu_mask(p.cpu_aff_mask);
        fprintf(stderr, "]\n");
        fprintf(stderr,
                "Video affinity:             unpinned  sched_getcpu=%d\n"
                "Audio affinity:             unpinned  sched_getcpu=%d\n"
                "Present affinity:           unpinned  sched_getcpu=%d\n"
                "Input affinity:             unpinned  sched_getcpu=%d\n"
                "Demux affinity:             unpinned  sched_getcpu=%d\n"
                "Controller events:          %lu  (print-only, no dvdnav yet)\n",
                p.sched_video_cpu, p.sched_audio_cpu, p.sched_present_cpu,
                p.sched_input_cpu, p.sched_demux_cpu, p.ctrl_events);
    }

    fprintf(stderr, "Raw mailbox v-a near start (ms):");
    for (int i = 0; i < p.first_raw_off_n; i++)
        fprintf(stderr, "  %+.1f", p.first_raw_offsets[i] / 1000.0);
    fprintf(stderr, "%s\n", p.first_raw_off_n ? "" : "  (none)");
    fprintf(stderr, "Raw mailbox v-a near end (ms):  ");
    for (int i = 0; i < p.last_raw_off_n; i++)
        fprintf(stderr, "  %+.1f", p.last_raw_offsets[i] / 1000.0);
    fprintf(stderr, "%s\n", p.last_raw_off_n ? "" : "  (none)");
    fprintf(stderr, "Presentation v-a near start (ms):");
    for (int i = 0; i < p.first_off_n; i++)
        fprintf(stderr, "  %+.1f", p.first_offsets[i] / 1000.0);
    fprintf(stderr, "%s\n", p.first_off_n ? "" : "  (none)");
    fprintf(stderr, "Presentation v-a near end (ms):  ");
    for (int i = 0; i < p.last_off_n; i++)
        fprintf(stderr, "  %+.1f", p.last_offsets[i] / 1000.0);
    fprintf(stderr, "%s\n", p.last_off_n ? "" : "  (none)");

    {
        int64_t T = p.diag_period_us;
        double ack_avg = p.ack_interval_n
                         ? (double)p.ack_interval_sum / p.ack_interval_n : 0.0;
        double m2a_avg = p.mbox_to_ack_n
                         ? (double)p.mbox_to_ack_sum / p.mbox_to_ack_n : 0.0;
        double lost_us = (T > 0) ? (double)p.ack_missed_periods * (double)T : 0.0;
        int64_t start_off = p.first_off_n ? p.first_offsets[0] : 0;
        int64_t end_off = p.last_offset;
        int64_t off_delta = end_off - start_off;
        int64_t raw_start_off = p.first_raw_off_n ? p.first_raw_offsets[0] : 0;
        int64_t raw_end_off = p.last_raw_offset;
        int64_t raw_off_delta = raw_end_off - raw_start_off;
        double start_mean = 0.0, end_mean = 0.0;
        double raw_start_mean = 0.0, raw_end_mean = 0.0;
        if (p.first_off_n) {
            int64_t s = 0;
            for (int i = 0; i < p.first_off_n; i++)
                s += p.first_offsets[i];
            start_mean = (double)s / p.first_off_n;
        }
        if (p.last_off_n) {
            int64_t s = 0;
            for (int i = 0; i < p.last_off_n; i++)
                s += p.last_offsets[i];
            end_mean = (double)s / p.last_off_n;
        }
        if (p.first_raw_off_n) {
            int64_t s = 0;
            for (int i = 0; i < p.first_raw_off_n; i++)
                s += p.first_raw_offsets[i];
            raw_start_mean = (double)s / p.first_raw_off_n;
        }
        if (p.last_raw_off_n) {
            int64_t s = 0;
            for (int i = 0; i < p.last_raw_off_n; i++)
                s += p.last_raw_offsets[i];
            raw_end_mean = (double)s / p.last_raw_off_n;
        }
        double expect_frames = 0.0;
        if (p.fps.num > 0 && p.fps.den > 0 && hw_dur > 0)
            expect_frames = hw_dur * (double)p.fps.num / (double)p.fps.den;

        fprintf(stderr,
                "\n=== display_buf ACK DIAGNOSTICS (not used for scheduling) ===\n"
                "Native frame period:        %.3f ms  (fps %d/%d)\n"
                "Ack transitions:            %lu\n"
                "Ack interval min/avg/max:   %.2f / %.2f / %.2f ms\n"
                "Normal ack intervals (<1.5T): %lu\n"
                "1-boundary misses (~2T):    %lu\n"
                "2-boundary misses (~3T):    %lu\n"
                ">2-boundary misses:         %lu\n"
                "Total inferred missed native periods: %" PRId64 "\n"
                "Inferred lost video time:   %.1f ms\n"
                "Mailbox->ack min/avg/max:   %.2f / %.2f / %.2f ms\n"
                "Mailbox->ack 0-1T / 1-2T / 2-3T / >3T: %lu / %lu / %lu / %lu\n"
                "ACK wait >1T/>2T/>3T/>200ms: %lu / %lu / %lu / %lu\n"
                "ACK wait max:               %.2f ms\n"
                "Request reissues:           %lu total in %lu waits\n"
                "ACK late warnings (>200ms): %lu\n"
                "ACK recovered after >200ms: %lu\n"
                "Last ACK wait:              want=%c  prev_display_buf=%s  "
                "reissues=%u  magic=%s\n"
                "Last ACK raw 0x30400008:    0x%016" PRIx64 "\n"
                "Final presentation v-a:     %+.2f ms\n"
                "Presentation v-a start->end:%+.1f -> %+.1f ms  (delta %+.1f ms)\n"
                "Presentation mean start/end:%+.1f / %+.1f ms  (delta %+.1f ms)\n"
                "Final raw mailbox v-a:      %+.2f ms\n"
                "Raw mailbox v-a start->end: %+.1f -> %+.1f ms  (delta %+.1f ms)\n"
                "Raw mailbox mean start/end: %+.1f / %+.1f ms  (delta %+.1f ms)\n"
                "Expected frames at fps:     %.3f\n"
                "Presented / stale-dropped:  %d / %d  (sum %d)\n"
                "Expected vs presented+stale: deficit %.3f\n"
                "Lost-time vs offset delta:  inferred %.1f ms vs start-end "
                "delta %+.1f ms\n",
                T / 1000.0, p.fps.num, p.fps.den,
                p.ack_n,
                p.ack_interval_min / 1000.0, ack_avg / 1000.0,
                p.ack_interval_max / 1000.0,
                p.ack_normal, p.ack_miss1, p.ack_miss2, p.ack_miss_gt2,
                p.ack_missed_periods,
                lost_us / 1000.0,
                p.mbox_to_ack_min / 1000.0, m2a_avg / 1000.0,
                p.mbox_to_ack_max / 1000.0,
                p.mbox_to_ack_0_1t, p.mbox_to_ack_1_2t,
                p.mbox_to_ack_2_3t, p.mbox_to_ack_gt3t,
                p.ackw_gt1t, p.ackw_gt2t, p.ackw_gt3t, p.ackw_gt200,
                p.ackw_max / 1000.0,
                p.ack_reissue_total, p.ack_wait_reissued,
                p.ack_late_n,
                p.ack_recovered_late_n,
                p.last_ack_want ? 'B' : 'A',
                p.last_ack_valid_ok
                    ? (p.last_ack_valid_buf ? "B" : "A") : "(none)",
                p.last_ack_reissues,
                p.last_ack_magic_ok ? "DVD1" : "invalid",
                p.last_ack_raw,
                end_off / 1000.0,
                start_off / 1000.0, end_off / 1000.0, off_delta / 1000.0,
                start_mean / 1000.0, end_mean / 1000.0,
                (end_mean - start_mean) / 1000.0,
                raw_end_off / 1000.0,
                raw_start_off / 1000.0, raw_end_off / 1000.0,
                raw_off_delta / 1000.0,
                raw_start_mean / 1000.0, raw_end_mean / 1000.0,
                (raw_end_mean - raw_start_mean) / 1000.0,
                expect_frames,
                p.rendered, p.stale_dropped, accounted,
                expect_frames - (double)accounted,
                lost_us / 1000.0, off_delta / 1000.0);
    }

    if (player_buffered(&p)) {
        fprintf(stderr,
                "\n=== BUFFERED STARTUP AUDIO ===\n"
                "Prefill requested/achieved: %d / %d\n"
                "Prefill wall time:          %.3f s  (%s)\n"
                "PCM hold cap:               %.1f ms  (%d bytes)\n"
                "PCM held at release:        %.1f ms  (%d bytes)\n"
                "PCM used for MrAudio fill:  %.1f ms  (%d bytes)\n"
                "PCM discarded/truncated:    %.1f ms  (%" PRId64 " bytes)\n"
                "First audio PTS (origin):   %.6f s\n",
                p.prefill_req, p.prefill_got,
                (p.prefill_t0_us && p.prefill_t1_us)
                    ? (p.prefill_t1_us - p.prefill_t0_us) / 1e6 : 0.0,
                p.prefill_reason ? p.prefill_reason : "(not released)",
                PCM_HOLD_START_BYTES * 1000.0 / (double)BYTES_PER_SEC,
                PCM_HOLD_START_BYTES,
                p.pcm_hold_at_release * 1000.0 / (double)BYTES_PER_SEC,
                p.pcm_hold_at_release,
                p.pcm_hold_used * 1000.0 / (double)BYTES_PER_SEC,
                p.pcm_hold_used,
                p.pcm_hold_discarded * 1000.0 / (double)BYTES_PER_SEC,
                p.pcm_hold_discarded,
                p.first_audio_pts_us == AV_NOPTS_VALUE
                    ? 0.0 : p.first_audio_pts_us / 1e6);
    }

    if (p.buffered_video) {
        const VidRing *r = &p.vring;
        double depth_avg = r->depth_n ? (double)r->depth_sum / r->depth_n : 0.0;
        double play_avg = r->play_depth_n
                          ? (double)r->play_depth_sum / r->play_depth_n : 0.0;
        int64_t Tbuf = frame_duration_us(&p);
        double prod_dur = 0.0;
        if (p.prod_t0_us && p.prod_t1_us >= p.prod_t0_us)
            prod_dur = (p.prod_t1_us - p.prod_t0_us) / 1e6;
        double prod_fps = (prod_dur > 0.0) ? p.prod_enqueued / prod_dur : 0.0;
        double prod_sws_avg = p.prod_sws_n
                              ? (double)p.prod_sws_sum / p.prod_sws_n : 0.0;
        double prod_cpu_avg = p.prod_sws_cpu_n
                              ? (double)p.prod_sws_cpu_sum / p.prod_sws_cpu_n
                              : 0.0;
        double prod_dec_avg = p.prod_dec_n
                              ? (double)p.prod_dec_sum / p.prod_dec_n : 0.0;
        double buf_ms = (Tbuf > 0) ? play_avg * (Tbuf / 1000.0) : 0.0;
        fprintf(stderr,
                "\n=== VIDEO BUFFER (cached RAM ring) ===\n"
                "Capacity:                   %d frames\n"
                "Prefill requested/achieved: %d / %d\n"
                "Prefill wall time:          %.3f s\n"
                "Prefill release reason:     %s\n"
                "Queue depth min/avg/max:    %d / %.1f / %d  (all samples)\n"
                "After-startup min/avg:      %d / %.1f\n"
                "Queue-empty events:         %lu\n"
                "Consumer wait-for-frame:    %.3f s\n"
                "Producer queue-full events: %lu\n"
                "Producer wait-for-space:    %.3f s\n"
                "Times queue at capacity:    %lu\n"
                "Pushed / popped:            %lu / %lu  (still queued %d)\n"
                "Avg buffered duration:      %.1f ms  (play-depth × T)\n"
                "Producer decode/arrival min/avg/max: %.2f / %.2f / %.2f ms"
                "  n=%lu\n"
                "Producer cached-sws wall min/avg/max: %.2f / %.2f / %.2f ms"
                "  n=%lu\n"
                "Producer cached-sws CPU min/avg/max:  %.2f / %.2f / %.2f ms"
                "  n=%lu\n"
                "Producer throughput:        %.3f fps  (enqueued/wall)\n",
                r->cap, p.prefill_req, p.prefill_got,
                (p.prefill_t0_us && p.prefill_t1_us)
                    ? (p.prefill_t1_us - p.prefill_t0_us) / 1e6 : 0.0,
                p.prefill_reason ? p.prefill_reason : "(not released)",
                r->depth_n ? r->depth_min : 0, depth_avg, r->depth_max,
                r->play_depth_n ? r->play_depth_min : 0, play_avg,
                r->empty_n, r->empty_wait_us / 1e6,
                r->full_n, r->full_block_us / 1e6,
                r->cap_hits, r->pushed, r->popped, r->count,
                buf_ms,
                p.prod_dec_min / 1000.0, prod_dec_avg / 1000.0,
                p.prod_dec_max / 1000.0, p.prod_dec_n,
                p.prod_sws_min / 1000.0, prod_sws_avg / 1000.0,
                p.prod_sws_max / 1000.0, p.prod_sws_n,
                p.prod_sws_cpu_min / 1000.0, prod_cpu_avg / 1000.0,
                p.prod_sws_cpu_max / 1000.0, p.prod_sws_cpu_n,
                prod_fps);
    } else if (p.buffered_yuv) {
        const YuvRing *r = &p.yuvring;
        double depth_avg = r->depth_n ? (double)r->depth_sum / r->depth_n : 0.0;
        double play_avg = r->play_depth_n
                          ? (double)r->play_depth_sum / r->play_depth_n : 0.0;
        int64_t Tbuf = frame_duration_us(&p);
        double prod_dur = 0.0;
        if (p.prod_t0_us && p.prod_t1_us >= p.prod_t0_us)
            prod_dur = (p.prod_t1_us - p.prod_t0_us) / 1e6;
        double prod_fps = (prod_dur > 0.0) ? p.prod_enqueued / prod_dur : 0.0;
        double prod_dec_avg = p.prod_dec_n
                              ? (double)p.prod_dec_sum / p.prod_dec_n : 0.0;
        double buf_ms = (Tbuf > 0) ? play_avg * (Tbuf / 1000.0) : 0.0;
        fprintf(stderr,
                "\n=== VIDEO BUFFER (decoded YUV AVFrame queue) ===\n"
                "Capacity:                   %d frames  (av_frame_ref, no "
                "plane copies)\n"
                "Prefill requested/achieved: %d / %d\n"
                "Prefill wall time:          %.3f s\n"
                "Prefill release reason:     %s\n"
                "Queue depth min/avg/max:    %d / %.1f / %d  (all samples)\n"
                "After-startup min/avg:      %d / %.1f\n"
                "Queue-empty events:         %lu\n"
                "Consumer wait-for-frame:    %.3f s\n"
                "Producer queue-full events: %lu\n"
                "Producer wait-for-space:    %.3f s\n"
                "Times queue at capacity:    %lu\n"
                "Pushed / popped:            %lu / %lu  (still queued %d)\n"
                "Avg buffered duration:      %.1f ms  (play-depth × T)\n"
                "Producer decode/arrival min/avg/max: %.2f / %.2f / %.2f ms"
                "  n=%lu\n"
                "Producer throughput:        %.3f fps  (enqueued/wall)\n",
                r->cap, p.prefill_req, p.prefill_got,
                (p.prefill_t0_us && p.prefill_t1_us)
                    ? (p.prefill_t1_us - p.prefill_t0_us) / 1e6 : 0.0,
                p.prefill_reason ? p.prefill_reason : "(not released)",
                r->depth_n ? r->depth_min : 0, depth_avg, r->depth_max,
                r->play_depth_n ? r->play_depth_min : 0, play_avg,
                r->empty_n, r->empty_wait_us / 1e6,
                r->full_n, r->full_block_us / 1e6,
                r->cap_hits, r->pushed, r->popped, r->count,
                buf_ms,
                p.prod_dec_min / 1000.0, prod_dec_avg / 1000.0,
                p.prod_dec_max / 1000.0, p.prod_dec_n,
                prod_fps);
    }

    if (p.perf_present_no_convert)
        print_perf_isolation_ack(&p);
    if (p.phase_decode)
        print_phase_decode(&p);

    {
        double dec_avg = p.dec_n ? (double)p.dec_sum / p.dec_n : 0.0;
        double ackw_avg = p.ackw_n ? (double)p.ackw_sum / p.ackw_n : 0.0;
        double inst_pct = p.ackw_n ? 100.0 * (double)p.ackw_instant / p.ackw_n : 0.0;
        double sws_avg = p.sws_n ? (double)p.sws_sum / p.sws_n : 0.0;
        double cpu_avg = p.sws_cpu_n ? (double)p.sws_cpu_sum / p.sws_cpu_n : 0.0;
        double pre_avg = p.sws_cpu_n ? (double)p.preempt_sum / p.sws_cpu_n : 0.0;
        double a2m_avg = p.a2m_n ? (double)p.a2m_sum / p.a2m_n : 0.0;
        double c2m_avg = p.c2m_n ? (double)p.c2m_sum / p.c2m_n : 0.0;
        double mcyc_avg = p.mbox_cyc_n ? (double)p.mbox_cyc_sum / p.mbox_cyc_n : 0.0;
        double cyc_avg = p.cyc_n ? (double)p.cyc_sum / p.cyc_n : 0.0;
        double memcpy_avg = p.memcpy_n ? (double)p.memcpy_sum / p.memcpy_n : 0.0;
        double memcpy_cpu_avg = p.memcpy_cpu_n
                                ? (double)p.memcpy_cpu_sum / p.memcpy_cpu_n
                                : 0.0;
        double prod_dec_avg2 = p.prod_dec_n
                               ? (double)p.prod_dec_sum / p.prod_dec_n : 0.0;
        if (p.buffered_video && p.memcpy_cpu_n)
            pre_avg = (double)p.preempt_sum / p.memcpy_cpu_n;
        int64_t Tfr = frame_duration_us(&p);
        double stale_off_avg = p.stale_n
                               ? (double)p.stale_off_sum / (double)p.stale_n : 0.0;

        fprintf(stderr,
                "\n=== STALE-FRAME RECOVERY ===\n"
                "Clock: MrAudio consumed-samples. Drop before ACK/%s/mailbox\n"
                "  when presentation_vpts + T <= aclk + %d us.\n"
                "  presentation_vpts = raw_vpts - N*T%s  (N=%d, advance=%.3f ms).\n"
                "Video frame duration T:     %.3f ms  (fps %d/%d)\n"
                "Stale threshold (pres v-a): %+.3f ms\n"
                "Timed frames decoded:       %d\n"
                "Presented:                  %d\n"
                "Stale-dropped:              %d\n"
                "Max consecutive drops:      %d\n"
                "v-a at drop min/avg/max:    %+.2f / %+.2f / %+.2f ms "
                "(presentation)\n",
                p.buffered_video ? "DDR memcpy" : "sws/DDR",
                EARLY_SLACK_US,
                p.buffered_yuv && p.video_advance_ms
                    ? " - video_advance" : "",
                p.initial_skip_req,
                video_advance_applied_us(&p) / 1000.0,
                Tfr / 1000.0, p.fps.num, p.fps.den,
                Tfr > 0 ? (EARLY_SLACK_US - Tfr) / 1000.0 : 0.0,
                timed_decoded, p.rendered, p.stale_dropped, p.stale_run_max,
                p.stale_n ? p.stale_off_min / 1000.0 : 0.0,
                stale_off_avg / 1000.0,
                p.stale_n ? p.stale_off_max / 1000.0 : 0.0);

        if (p.buffered_video) {
            fprintf(stderr,
                    "\n=== VIDEO CRITICAL PATH DIAGNOSTICS ===\n"
                    "Video pipeline: BUFFERED CACHED-RAM producer/consumer\n"
                    "Consumer path: queue pop → stale-check → ACK wait → "
                    "memcpy RAM→DDR →\n"
                    "  barrier → PTS +2 ms → mailbox. Decode+sws are off this "
                    "path.\n"
                    "ACK wait: wait_display_buf for the previous mailbox dest.\n"
                    "Cycle: consumer present from before ACK wait through "
                    "mailbox.\n"
                    "ACK-to-mailbox: dest chosen → mailbox (memcpy + PTS hold).\n"
                    "Convert-done-to-mailbox: after barrier → mailbox "
                    "(hold + overhead).\n"
                    "Mailbox cycle: previous mailbox write → this mailbox "
                    "write.\n"
                    "memcpy CPU: %s\n"
                    "Producer decode/arrival min/avg/max: %.2f / %.2f / %.2f ms"
                    "  n=%lu\n"
                    "ACK already satisfied:      %lu / %lu  (%.1f%%)\n"
                    "ACK wait min/avg/max:       %.2f / %.2f / %.2f ms\n"
                    "ACK wait >5/>10/>20/>30/>40 ms: %lu / %lu / %lu / %lu / "
                    "%lu\n"
                    "RAM→DDR memcpy wall min/avg/max: %.2f / %.2f / %.2f ms\n"
                    "memcpy >20/>25/>30/>35/>40/>50 ms: %lu / %lu / %lu / %lu / "
                    "%lu / %lu\n",
                    p.sws_cpu_ok ? "CLOCK_THREAD_CPUTIME_ID"
                                 : "unavailable (omitted)",
                    p.prod_dec_min / 1000.0, prod_dec_avg2 / 1000.0,
                    p.prod_dec_max / 1000.0, p.prod_dec_n,
                    p.ackw_instant, p.ackw_n, inst_pct,
                    p.ackw_min / 1000.0, ackw_avg / 1000.0, p.ackw_max / 1000.0,
                    p.ackw_gt5, p.ackw_gt10, p.ackw_gt20, p.ackw_gt30,
                    p.ackw_gt40,
                    p.memcpy_min / 1000.0, memcpy_avg / 1000.0,
                    p.memcpy_max / 1000.0,
                    p.memcpy_gt20, p.memcpy_gt25, p.memcpy_gt30, p.memcpy_gt35,
                    p.memcpy_gt40, p.memcpy_gt50);
            if (p.sws_cpu_ok) {
                fprintf(stderr,
                        "RAM→DDR memcpy CPU min/avg/max: %.2f / %.2f / %.2f ms"
                        "  n=%lu\n"
                        "Preemption gap min/avg/max: %.2f / %.2f / %.2f ms\n"
                        "Preemption >1/>2/>5/>10 ms: %lu / %lu / %lu / %lu\n",
                        p.memcpy_cpu_min / 1000.0, memcpy_cpu_avg / 1000.0,
                        p.memcpy_cpu_max / 1000.0, p.memcpy_cpu_n,
                        p.preempt_min / 1000.0, pre_avg / 1000.0,
                        p.preempt_max / 1000.0,
                        p.preempt_gt1, p.preempt_gt2, p.preempt_gt5,
                        p.preempt_gt10);
            } else {
                fprintf(stderr,
                        "RAM→DDR memcpy CPU min/avg/max: n/a\n"
                        "Preemption gap min/avg/max: n/a\n");
            }
        } else if (p.buffered_yuv) {
            fprintf(stderr,
                    "\n=== VIDEO CRITICAL PATH DIAGNOSTICS ===\n"
                    "Video pipeline: BUFFERED YUV producer / direct-DDR sws "
                    "consumer\n"
                    "Consumer path: queue pop → stale-check → ACK wait → "
                    "sws YUV DIRECT DDR →\n"
                    "  barrier → PTS +2 ms → mailbox. Decode is off this path.\n"
                    "ACK wait: wait_display_buf for the previous mailbox dest.\n"
                    "Cycle: consumer present from before ACK wait through "
                    "mailbox.\n"
                    "ACK-to-mailbox: dest chosen → mailbox (direct-DDR sws + "
                    "PTS hold).\n"
                    "Convert-done-to-mailbox: after barrier → mailbox "
                    "(hold + overhead).\n"
                    "Mailbox cycle: previous mailbox write → this mailbox "
                    "write.\n"
                    "sws thread CPU: %s\n"
                    "Producer decode/arrival min/avg/max: %.2f / %.2f / %.2f ms"
                    "  n=%lu  (off present path)\n"
                    "ACK already satisfied:      %lu / %lu  (%.1f%%)\n"
                    "ACK wait min/avg/max:       %.2f / %.2f / %.2f ms\n"
                    "ACK wait >5/>10/>20/>30/>40 ms: %lu / %lu / %lu / %lu / "
                    "%lu\n"
                    "direct-DDR sws wall min/avg/max: %.2f / %.2f / %.2f ms\n"
                    "sws >20/>25/>30/>35/>40/>50 ms: %lu / %lu / %lu / %lu / "
                    "%lu / %lu\n",
                    p.sws_cpu_ok ? "CLOCK_THREAD_CPUTIME_ID"
                                 : "unavailable (omitted)",
                    p.prod_dec_min / 1000.0, prod_dec_avg2 / 1000.0,
                    p.prod_dec_max / 1000.0, p.prod_dec_n,
                    p.ackw_instant, p.ackw_n, inst_pct,
                    p.ackw_min / 1000.0, ackw_avg / 1000.0, p.ackw_max / 1000.0,
                    p.ackw_gt5, p.ackw_gt10, p.ackw_gt20, p.ackw_gt30,
                    p.ackw_gt40,
                    p.sws_min / 1000.0, sws_avg / 1000.0, p.sws_max / 1000.0,
                    p.sws_gt20, p.sws_gt25, p.sws_gt30, p.sws_gt35,
                    p.sws_gt40, p.sws_gt50);
            if (p.sws_cpu_ok) {
                fprintf(stderr,
                        "direct-DDR sws CPU min/avg/max: %.2f / %.2f / %.2f ms"
                        "  n=%lu\n"
                        "Preemption gap min/avg/max: %.2f / %.2f / %.2f ms\n"
                        "Preemption >1/>2/>5/>10 ms: %lu / %lu / %lu / %lu\n",
                        p.sws_cpu_min / 1000.0, cpu_avg / 1000.0,
                        p.sws_cpu_max / 1000.0, p.sws_cpu_n,
                        p.preempt_min / 1000.0, pre_avg / 1000.0,
                        p.preempt_max / 1000.0,
                        p.preempt_gt1, p.preempt_gt2, p.preempt_gt5,
                        p.preempt_gt10);
            } else {
                fprintf(stderr,
                        "direct-DDR sws CPU min/avg/max: n/a\n"
                        "Preemption gap min/avg/max: n/a\n");
            }
        } else {
        fprintf(stderr,
                "\n=== VIDEO CRITICAL PATH DIAGNOSTICS ===\n"
                "Pipeline: decode → stale-check → wait previous display_buf ACK →\n"
                "  sws_scale DIRECTLY into inactive DDR → barrier → PTS +2 ms →\n"
                "  mailbox. Threads unpinned. Stale frames skip this path.\n"
                "Decode/frame-arrival: wall from previous mailbox return to\n"
                "  avcodec_receive_frame (presented frames only).\n"
                "ACK wait: wait_display_buf for the previous mailbox dest.\n"
                "  Instant = already matched on the first poll.\n"
                "Cycle: present_video_frame from before that wait through mailbox.\n"
                "ACK-to-mailbox: dest chosen → mailbox (direct-DDR sws + PTS hold).\n"
                "Convert-done-to-mailbox: after barrier → mailbox (hold + overhead).\n"
                "Mailbox cycle: previous mailbox write → this mailbox write.\n"
                "sws thread CPU: %s\n"
                "Frame/decode availability min/avg/max: %.2f / %.2f / %.2f ms  n=%lu\n"
                "Decode >10/>20/>30/>40 ms:  %lu / %lu / %lu / %lu\n"
                "ACK already satisfied:      %lu / %lu  (%.1f%%)\n"
                "ACK wait min/avg/max:       %.2f / %.2f / %.2f ms\n"
                "ACK wait >5/>10/>20/>30/>40 ms: %lu / %lu / %lu / %lu / %lu\n"
                "sws_scale wall min/avg/max: %.2f / %.2f / %.2f ms\n"
                "sws >20/>25/>30/>35/>40/>50 ms: %lu / %lu / %lu / %lu / %lu / %lu\n",
                p.sws_cpu_ok ? "CLOCK_THREAD_CPUTIME_ID" : "unavailable (omitted)",
                p.dec_min / 1000.0, dec_avg / 1000.0, p.dec_max / 1000.0, p.dec_n,
                p.dec_gt10, p.dec_gt20, p.dec_gt30, p.dec_gt40,
                p.ackw_instant, p.ackw_n, inst_pct,
                p.ackw_min / 1000.0, ackw_avg / 1000.0, p.ackw_max / 1000.0,
                p.ackw_gt5, p.ackw_gt10, p.ackw_gt20, p.ackw_gt30, p.ackw_gt40,
                p.sws_min / 1000.0, sws_avg / 1000.0, p.sws_max / 1000.0,
                p.sws_gt20, p.sws_gt25, p.sws_gt30, p.sws_gt35,
                p.sws_gt40, p.sws_gt50);
        if (p.sws_cpu_ok) {
            fprintf(stderr,
                    "sws_scale CPU min/avg/max:  %.2f / %.2f / %.2f ms\n"
                    "Preemption gap min/avg/max: %.2f / %.2f / %.2f ms\n"
                    "Preemption >1/>2/>5/>10 ms: %lu / %lu / %lu / %lu\n",
                    p.sws_cpu_min / 1000.0, cpu_avg / 1000.0,
                    p.sws_cpu_max / 1000.0,
                    p.preempt_min / 1000.0, pre_avg / 1000.0,
                    p.preempt_max / 1000.0,
                    p.preempt_gt1, p.preempt_gt2, p.preempt_gt5, p.preempt_gt10);
        } else {
            fprintf(stderr,
                    "sws_scale CPU min/avg/max:  n/a\n"
                    "Preemption gap min/avg/max: n/a\n"
                    "Preemption >1/>2/>5/>10 ms: n/a\n");
        }
        }
        fprintf(stderr,
                "ACK-to-mailbox min/avg/max: %.2f / %.2f / %.2f ms\n"
                "ACK-to-mailbox >20/>30/>35/>40/>50 ms: %lu / %lu / %lu / %lu / %lu\n"
                "Convert-done-to-mailbox min/avg/max: %.2f / %.2f / %.2f ms\n"
                "Present cycle min/avg/max:  %.2f / %.2f / %.2f ms\n"
                "Present cycle >30/>35/>40/>50/>80 ms: %lu / %lu / %lu / %lu / %lu\n"
                "Mailbox cycle min/avg/max:  %.2f / %.2f / %.2f ms  n=%lu\n"
                "%s:           %.2f ms  (%s budget %.3f ms)\n"
                "Inferred missed native boundaries: %u\n",
                p.a2m_min / 1000.0, a2m_avg / 1000.0, p.a2m_max / 1000.0,
                p.a2m_gt20, p.a2m_gt30, p.a2m_gt35, p.a2m_gt40, p.a2m_gt50,
                p.c2m_min / 1000.0, c2m_avg / 1000.0, p.c2m_max / 1000.0,
                p.cyc_min / 1000.0, cyc_avg / 1000.0, p.cyc_max / 1000.0,
                p.cyc_gt30, p.cyc_gt35, p.cyc_gt40, p.cyc_gt50, p.cyc_gt80,
                p.mbox_cyc_min / 1000.0, mcyc_avg / 1000.0, p.mbox_cyc_max / 1000.0,
                p.mbox_cyc_n,
                p.buffered_yuv ? "Present cycle avg (decode off path)"
                               : "Decode+cycle avg",
                p.buffered_yuv ? cyc_avg / 1000.0
                               : (dec_avg + cyc_avg) / 1000.0,
                dvd_std_name(p.dvd_std),
                Tfr / 1000.0,
                p.miss_total);
        if (p.fpga_yuv420) {
            fprintf(stderr, "yuv_copy_us:\n");
            subperf_print_dist("yuv_copy_us", p.yuv_copy, p.yuv_copy_n,
                               p.yuv_copy_sum, p.yuv_copy_max);
        }
        if (p.miss_log_n) {
            unsigned i, n = p.miss_log_n;
            unsigned start = (p.miss_total > MISS_LOG_CAP) ? p.miss_log_i : 0;
            fprintf(stderr, "Per-miss correlation (last %u of %u):\n",
                    n, p.miss_total);
            for (i = 0; i < n; i++) {
                unsigned idx = (start + i) % MISS_LOG_CAP;
                print_miss_rec(&p, p.misses[idx].miss_n, p.misses[idx].frame,
                               p.misses[idx].vpts_us, p.misses[idx].av_off_us,
                               p.misses[idx].ack_iv_us, p.misses[idx].ack_wait_us,
                               p.misses[idx].ack_instant, p.misses[idx].decode_us,
                               p.misses[idx].sws_wall_us, p.misses[idx].sws_cpu_us,
                               p.misses[idx].preempt_us,
                               p.misses[idx].ack_to_mbox_us,
                               p.misses[idx].conv_to_mbox_us,
                               p.misses[idx].mbox_to_ack_us,
                               p.misses[idx].cycle_us);
            }
        } else {
            fprintf(stderr, "Per-miss correlation: none\n");
        }
    }
    }
    }

    if (read_ret < 0 && read_ret != AVERROR_EOF) {
        char e[128];
        fferr(read_ret, e, sizeof(e));
        fprintf(stderr, "Demux ended: %s\n", e);
    } else if (read_ret < 0) {
        char e[128];
        fferr(read_ret, e, sizeof(e));
        dbg("Demux ended: %s\n", e);
    }

    pktq_free(&p.aq);
    pktq_free(&p.vq);
    if (player_buffered(&p)) {
        vidring_free(&p.vring);
        yuvring_free(&p.yuvring);
        pthread_mutex_destroy(&p.prefill_mu);
        pthread_cond_destroy(&p.prefill_cv);
        av_free(p.pcm_hold);
    }
    if (p.phase_inited) {
        pthread_mutex_destroy(&p.phase_mu);
        pthread_cond_destroy(&p.phase_cv);
        free(p.phase_sws);
        free(p.phase_ack);
        free(p.phase_cyc);
    }
    free(p.yuv_copy);
    free(p.subperf.act_blend);
    free(p.subperf.act_sws);
    free(p.subperf.act_ack);
    free(p.subperf.act_cyc);
    free(p.subperf.act_sws_sub);
    free(p.subperf.inact_sws);
    free(p.subperf.inact_ack);
    free(p.subperf.inact_cyc);
    avcodec_parameters_free(&p.acp);
    avcodec_parameters_free(&p.vcp);
    navq_destroy(&p);
    pthread_mutex_destroy(&p.clock.mu);
    pthread_cond_destroy(&p.clock.ready_cv);
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    avio_context_free(&avio);
    dvdnav_close(d.nav);
    free(d.sector);
    if (p.fpga_v1_caps)
        dvd_fpga_write_source(&fb, FPGA_SRC_UNKNOWN);
    unmap_double_fb(&fb);

    if (g_interrupt) {
        dbg("\nStopped by signal (%.1f s wall / %.1f s audio).\n",
            wall, hw_dur);
        return 130;
    }
    if (p.fail)
        return 10;
    if (p.uncapped_bench) {
        if (p.bench_converted <= 0)
            return 9;
        dbg("\nPASS: uncapped video benchmark finished "
            "(%.1f s wall, %d frames).\n",
            wall, p.bench_converted);
        return 0;
    }
    if (p.rendered <= 0 || p.out_samples <= 0)
        return 9;
    dbg("\nPASS: playback finished (%.1f s wall / %.1f s audio).\n",
        wall, hw_dur);
    return 0;
}
