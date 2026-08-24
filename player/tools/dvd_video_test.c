/*
 * dvd_video_test.c — PTS-paced DVD -> MISTER_FB presentation test.
 *
 * Pipeline (500 consecutive frames, PTS-paced):
 *   /dev/sr0 -> libdvdnav -> custom AVIO -> MPEG-PS demux
 *   -> explicit MPEG-2 ES parser (required architecture)
 *   -> MPEG-2 decode -> wait until the frame's PTS on a monotonic clock
 *   -> swscale YUV->BGR0 at native 720x576 written DIRECTLY into the
 *      mapped framebuffer at 0x30000000 (stride 2880).
 *
 * Playback clock: first genuine DVD PTS anchors a continuous timeline.
 * Frames without a PTS are given interpolated timestamps at the detected
 * MPEG-2 frame duration. A later genuine PTS validates that timeline and
 * may slew it by at most one frame; it does not jump presentation.
 * 25 fps is used only if neither frame rate nor timestamps are available.
 *
 * Deliberately NOT implemented: audio, controls, menus, double
 * buffering, NTSC, vsync page-flip.
 *
 * Requires: DVD core loaded on the SS1 and a PAL DVD in /dev/sr0.
 */

#define _GNU_SOURCE

#include <dvdnav/dvdnav.h>
#include <dvdnav/dvdnav_events.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/cpu.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#define DVD_SECTOR    2048
#define AVIO_BUF_SIZE (64 * 1024)

/* Consecutive decoded frames to push through the full path. */
#define TARGET_FRAMES 500

/* Used only when a decoded frame has no PTS. */
#define FALLBACK_FPS  25

/* Proven MISTER_FB framebuffer geometry (fpga/DVD.sv, tag working-mister-fb). */
#define FB_PHYS_BASE  0x30000000UL
#define FB_W          720
#define FB_H          576
#define FB_STRIDE     2880                        /* bytes per line          */
#define FB_SIZE       ((size_t)FB_STRIDE * FB_H)  /* 0x195000 = 1,658,880    */

/* ------------------------------------------------------------------ */
/* SIGILL diagnostics (from dvdplayer_headless.c)                      */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t stage = 0;

static const char *stage_names[] = {
    "startup",
    "dvdnav",
    "mpeg demux",
    "packet read",
    "video decoder",
    "frame decode",
    "ddr framebuffer",
    "swscale/copy"
};

static void sigill_handler(int sig, siginfo_t *si, void *ctxv)
{
    (void)sig;

    ucontext_t *uc = (ucontext_t *)ctxv;
    unsigned long pc = 0;
    unsigned long lr = 0;

#if defined(__arm__)
    pc = (unsigned long)uc->uc_mcontext.arm_pc;
    lr = (unsigned long)uc->uc_mcontext.arm_lr;
#endif

    dprintf(STDERR_FILENO, "\n*** SIGILL / illegal instruction ***\n");

    dprintf(STDERR_FILENO, "Stage %d: %s\n", (int)stage,
            (stage >= 0 &&
             stage < (int)(sizeof(stage_names) / sizeof(stage_names[0])))
                ? stage_names[stage]
                : "unknown");

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

/* ------------------------------------------------------------------ */
/* DVD -> AVIO plumbing (from dvdplayer_headless.c)                    */
/* ------------------------------------------------------------------ */

typedef struct {
    dvdnav_t *nav;

    uint8_t *sector;
    int sector_len;
    int sector_pos;
    int stopped;

    unsigned long nav_packets;
    unsigned long mpeg_sectors;
} DVDIO;

static void fferr(int err, char *buf, size_t n)
{
    if (av_strerror(err, buf, n) < 0)
        snprintf(buf, n, "FFmpeg error %d", err);
}

static int dvd_read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    DVDIO *d = opaque;
    int out = 0;

    while (out < buf_size) {

        if (d->stopped)
            return out ? out : AVERROR_EOF;

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

        int32_t event = 0;
        int32_t len = 0;

        dvdnav_status_t st =
            dvdnav_get_next_block(d->nav, d->sector, &event, &len);

        if (st != DVDNAV_STATUS_OK) {

            fprintf(stderr, "dvdnav error: %s\n",
                    dvdnav_err_to_string(d->nav));

            d->stopped = 1;

            return out ? out : AVERROR_EOF;
        }

        switch (event) {

            case DVDNAV_NAV_PACKET:

                d->nav_packets++;

                if (len == DVD_SECTOR) {
                    d->sector_len = len;
                    d->sector_pos = 0;
                }

                continue;

            case DVDNAV_BLOCK_OK:

                if (len == DVD_SECTOR) {
                    d->sector_len = len;
                    d->sector_pos = 0;
                    d->mpeg_sectors++;
                }

                continue;

            case DVDNAV_WAIT:

                dvdnav_wait_skip(d->nav);
                continue;

            case DVDNAV_STILL_FRAME:

                dvdnav_still_skip(d->nav);
                continue;

            case DVDNAV_VTS_CHANGE:
            case DVDNAV_HOP_CHANNEL:

                continue;

            case DVDNAV_STOP:

                d->stopped = 1;
                return out ? out : AVERROR_EOF;

            default:

                continue;
        }
    }

    return out;
}

static AVCodecContext *open_video_decoder(AVStream *st)
{
    stage = 4;

    AVCodecParameters *cp = st->codecpar;

    if (cp->codec_id == AV_CODEC_ID_NONE)
        cp->codec_id = AV_CODEC_ID_MPEG2VIDEO;

    const AVCodec *codec = avcodec_find_decoder(cp->codec_id);

    if (!codec) {
        fprintf(stderr, "MPEG-2 decoder unavailable\n");
        return NULL;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);

    if (!ctx)
        return NULL;

    if (avcodec_parameters_to_context(ctx, cp) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }

    ctx->pkt_timebase = st->time_base;

    int r = avcodec_open2(ctx, codec, NULL);

    fprintf(stderr, "avcodec_open2(%s) returned %d\n", codec->name, r);

    if (r < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }

    return ctx;
}

static AVRational frame_sar(AVFrame *frame, AVCodecContext *ctx)
{
    AVRational sar = frame->sample_aspect_ratio;

    if (sar.num <= 0 || sar.den <= 0)
        sar = ctx->sample_aspect_ratio;

    if (sar.num <= 0 || sar.den <= 0)
        sar = (AVRational){1, 1};

    return sar;
}

/* ------------------------------------------------------------------ */
/* MISTER_FB DDR framebuffer access (from ddr_fb_writer.c)             */
/* ------------------------------------------------------------------ */

/*
 * Return 1 if [base, base+size) overlaps a "System RAM" range in
 * /proc/iomem, 0 if no overlap found, -1 if the check was not possible.
 */
static int overlaps_system_ram(unsigned long base, size_t size)
{
    FILE *fp = fopen("/proc/iomem", "r");

    if (!fp)
        return -1;

    char line[256];
    int checked = 0;
    int overlap = 0;

    while (fgets(line, sizeof(line), fp)) {

        if (!strstr(line, "System RAM"))
            continue;

        unsigned long long start = 0, end = 0;

        /* Format: "00000000-1fffffff : System RAM" (possibly indented). */
        if (sscanf(line, " %llx-%llx :", &start, &end) != 2)
            continue;

        /* Root-only detail hidden => ranges read as 0-0; can't verify. */
        if (start == 0 && end == 0)
            continue;

        checked = 1;

        fprintf(stderr, "  System RAM: 0x%09llx-0x%09llx\n", start, end);

        if (base <= end && (base + size - 1) >= start)
            overlap = 1;
    }

    fclose(fp);

    if (!checked)
        return -1;

    return overlap;
}

/* Returns mapped pointer or NULL. Prints its own diagnostics. */
static uint8_t *map_mister_fb(int *fd_out)
{
    stage = 6;

    fprintf(stderr,
            "\nMapping MISTER_FB DDR framebuffer:\n"
            "Target range: 0x%08lx-0x%08lx (%zu bytes, stride %d)\n",
            FB_PHYS_BASE, FB_PHYS_BASE + FB_SIZE - 1, FB_SIZE, FB_STRIDE);

    fprintf(stderr, "Checking /proc/iomem for System RAM overlap...\n");

    int ov = overlaps_system_ram(FB_PHYS_BASE, FB_SIZE);

    if (ov > 0) {
        fprintf(stderr,
                "FAIL: 0x%08lx overlaps Linux System RAM. Refusing to write.\n",
                FB_PHYS_BASE);
        return NULL;
    }
    if (ov < 0)
        fprintf(stderr,
                "WARNING: could not verify via /proc/iomem (unreadable or "
                "hidden).\nProceeding on the documented MiSTer memory map.\n");
    else
        fprintf(stderr, "OK: no overlap with System RAM.\n");

    int fd = open("/dev/mem", O_RDWR | O_SYNC);

    if (fd < 0) {
        fprintf(stderr, "open /dev/mem: %s (need root)\n", strerror(errno));
        return NULL;
    }

    uint8_t *mem = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, (off_t)FB_PHYS_BASE);

    if (mem == MAP_FAILED) {
        fprintf(stderr, "mmap 0x%08lx: %s\n", FB_PHYS_BASE, strerror(errno));
        close(fd);
        return NULL;
    }

    fprintf(stderr, "Mapped %zu bytes at 0x%08lx.\n", FB_SIZE, FB_PHYS_BASE);

    *fd_out = fd;
    return mem;
}

/* ------------------------------------------------------------------ */
/* Per-frame rendering: swscale writes BGR0 directly into mapped DDR   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *fb;                  /* mapped MISTER_FB DDR buffer        */

    struct SwsContext *sws;
    uint8_t *dst_data[4];
    int dst_linesize[4];

    int rendered;

    int64_t convert_us;
    int64_t wait_us;

    /* PTS / frame-rate timeline (stream time_base). */
    AVRational tb;
    AVRational avg_frame_rate;
    AVRational r_frame_rate;
    AVRational fps;

    int64_t first_genuine_pts;
    int64_t timeline_pts;
    int64_t assigned_pts;
    int64_t origin_us;
    int64_t last_target_us;

    int genuine_pts_count;
    int interpolated_count;
    int fallback_frames;
    int64_t max_disc_us;

    int64_t first_present_us;
    int64_t last_present_us;
    int64_t first_display_pts;
    int64_t last_display_pts;
    int64_t first_convert_us;
    int64_t wait_us_at_first;

    int late_frames;
    int64_t late_us_sum;
    int64_t late_us_max;

    int fail;                     /* geometry/setup violation           */
} Render;

static AVRational detect_fps(const Render *rc, const AVCodecContext *vdec)
{
    if (vdec && vdec->framerate.num > 0 && vdec->framerate.den > 0)
        return vdec->framerate;

    if (vdec && vdec->time_base.num > 0 && vdec->time_base.den > 0 &&
        vdec->ticks_per_frame > 0) {
        return av_make_q(vdec->time_base.den,
                         vdec->time_base.num * vdec->ticks_per_frame);
    }

    if (rc->avg_frame_rate.num > 0 && rc->avg_frame_rate.den > 0)
        return rc->avg_frame_rate;

    if (rc->r_frame_rate.num > 0 && rc->r_frame_rate.den > 0)
        return rc->r_frame_rate;

    return (AVRational){0, 0};
}

static int64_t frame_duration_tb(const Render *rc)
{
    if (rc->fps.num > 0 && rc->fps.den > 0 &&
        rc->tb.num > 0 && rc->tb.den > 0)
        return av_rescale_q(1, av_inv_q(rc->fps), rc->tb);

    return 0;
}

static int64_t tb_to_us(const Render *rc, int64_t ticks)
{
    return av_rescale_q(ticks, rc->tb, (AVRational){1, 1000000});
}

/*
 * Continuous presentation timeline. The first genuine DVD PTS is the
 * anchor. Missing timestamps are interpolated at the detected frame
 * duration. A later genuine PTS is compared to that interpolation and
 * may slew the clock by at most one frame; presentation is not jumped.
 */
static void pace_until_pts(Render *rc, const AVFrame *frame,
                           const AVCodecContext *vdec)
{
    AVRational detected = detect_fps(rc, vdec);

    if (detected.num > 0 && detected.den > 0)
        rc->fps = detected;

    const int64_t genuine =
        (frame->pts != AV_NOPTS_VALUE) ? frame->pts : AV_NOPTS_VALUE;
    const int64_t now = av_gettime_relative();
    const int64_t dur = frame_duration_tb(rc);

    int64_t timeline = AV_NOPTS_VALUE;
    int64_t target;

    if (genuine != AV_NOPTS_VALUE && rc->tb.num > 0 && rc->tb.den > 0) {

        rc->genuine_pts_count++;

        if (rc->first_genuine_pts == AV_NOPTS_VALUE) {

            rc->first_genuine_pts = genuine;
            rc->timeline_pts = genuine;
            rc->origin_us = now;
            timeline = genuine;
            target = now;

        } else if (dur > 0) {

            int64_t expected = rc->timeline_pts + dur;
            int64_t disc = genuine - expected;
            int64_t disc_us = tb_to_us(rc, disc);

            if (disc_us < 0)
                disc_us = -disc_us;

            if (disc_us > rc->max_disc_us)
                rc->max_disc_us = disc_us;

            /* Show this frame on the interpolated clock (no jump). */
            timeline = expected;

            if (disc > dur)
                disc = dur;
            else if (disc < -dur)
                disc = -dur;

            rc->timeline_pts = expected + disc;

            target = rc->origin_us +
                     tb_to_us(rc, timeline - rc->first_genuine_pts);

        } else {

            timeline = genuine;
            rc->timeline_pts = genuine;
            target = rc->origin_us +
                     tb_to_us(rc, genuine - rc->first_genuine_pts);
        }

    } else if (rc->first_genuine_pts != AV_NOPTS_VALUE && dur > 0) {

        rc->interpolated_count++;
        timeline = rc->timeline_pts + dur;
        rc->timeline_pts = timeline;
        target = rc->origin_us +
                 tb_to_us(rc, timeline - rc->first_genuine_pts);

    } else if (rc->fps.num > 0 && rc->fps.den > 0) {

        rc->interpolated_count++;

        if (rc->last_target_us > 0)
            target = rc->last_target_us +
                     av_rescale_q(1, av_inv_q(rc->fps),
                                  (AVRational){1, 1000000});
        else
            target = now;

    } else if (rc->last_target_us > 0) {

        rc->fallback_frames++;
        target = rc->last_target_us + 1000000 / FALLBACK_FPS;

    } else {

        rc->fallback_frames++;
        target = now;
    }

    rc->assigned_pts = timeline;
    rc->last_target_us = target;

    if (now < target) {

        av_usleep((unsigned)(target - now));
        rc->wait_us += av_gettime_relative() - now;

    } else if (now > target) {

        int64_t late = now - target;

        rc->late_frames++;
        rc->late_us_sum += late;

        if (late > rc->late_us_max)
            rc->late_us_max = late;
    }
}

static void render_frame(Render *rc, AVFrame *frame, AVCodecContext *vdec)
{
    /* Native resolution only: refuse anything that is not 720x576. */
    if (frame->width != FB_W || frame->height != FB_H) {

        fprintf(stderr,
                "\nFAIL: frame %d is %dx%d, expected exactly %dx%d.\n"
                "No resizing allowed in this test.\n",
                rc->rendered + 1, frame->width, frame->height, FB_W, FB_H);

        rc->fail = 1;
        return;
    }

    /* First frame: set up conversion once and report stream properties. */
    if (!rc->sws) {

        AVRational sar = frame_sar(frame, vdec);
        AVRational fps0 = detect_fps(rc, vdec);

        fprintf(stderr,
                "\n=== VIDEO PATH STARTING ===\n"
                "Dimensions:          %dx%d\n"
                "Pixel format:        %s\n"
                "Sample aspect ratio: %d:%d\n"
                "swscale dest:        mapped DDR (stride %d)\n"
                "Detected frame rate: %d/%d (%.3f fps)\n"
                "Pacing:              continuous PTS + interpolation\n"
                "Pushing %d paced frames...\n\n",
                frame->width, frame->height,
                av_get_pix_fmt_name(frame->format)
                    ? av_get_pix_fmt_name(frame->format)
                    : "unknown",
                sar.num, sar.den,
                FB_STRIDE,
                fps0.num, fps0.den,
                (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0,
                TARGET_FRAMES);

        /* Destination is the proven MISTER_FB mapping, not a temp buffer. */
        rc->dst_data[0] = rc->fb;
        rc->dst_linesize[0] = FB_STRIDE;

        rc->sws = sws_getContext(FB_W, FB_H, frame->format,
                                 FB_W, FB_H, AV_PIX_FMT_BGR0,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);

        if (!rc->sws) {
            fprintf(stderr, "Could not create swscale context\n");
            rc->fail = 1;
            return;
        }
    }

    pace_until_pts(rc, frame, vdec);

    stage = 7;

    int64_t t0 = av_gettime_relative();

    sws_scale(rc->sws,
              (const uint8_t * const *)frame->data, frame->linesize,
              0, FB_H,
              rc->dst_data, rc->dst_linesize);

    int64_t t1 = av_gettime_relative();

    stage = 5;

    rc->convert_us += t1 - t0;

    /* Presentation instant = completion of the single-buffer DDR write. */
    {
        int64_t display_pts = rc->assigned_pts;

        if (!rc->first_present_us) {
            rc->first_present_us = t1;
            rc->first_display_pts = display_pts;
            rc->first_convert_us = t1 - t0;
            rc->wait_us_at_first = rc->wait_us;
        }

        if (rc->first_display_pts == AV_NOPTS_VALUE &&
            display_pts != AV_NOPTS_VALUE)
            rc->first_display_pts = display_pts;

        rc->last_present_us = t1;
        rc->last_display_pts = display_pts;
    }

    rc->rendered++;

    if ((rc->rendered % 100) == 0)
        fprintf(stderr, "Displayed %d/%d frames\n",
                rc->rendered, TARGET_FRAMES);
}

/*
 * Send one parsed packet (or NULL to flush) to the decoder and render
 * every decoded frame. Returns 1 when done (target reached or failure).
 */
static int decode_and_render(AVCodecContext *vdec,
                             AVPacket *ppkt,
                             AVFrame *frame,
                             Render *rc)
{
    if (avcodec_send_packet(vdec, ppkt) < 0)
        return 0;

    while (rc->rendered < TARGET_FRAMES && !rc->fail &&
           avcodec_receive_frame(vdec, frame) == 0) {

        render_frame(rc, frame, vdec);

        av_frame_unref(frame);
    }

    return rc->rendered >= TARGET_FRAMES || rc->fail;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    install_sigill_handler();

    setvbuf(stderr, NULL, _IONBF, 0);

    const int64_t program_start = av_gettime_relative();

    fprintf(stderr, "=== SS1 DVD -> MISTER_FB PTS-PACED VIDEO TEST ===\n");
    fprintf(stderr, "FFmpeg CPU flags: 0x%x\n", av_get_cpu_flags());

    const char *dvd = argc > 1 ? argv[1] : "/dev/sr0";

    /* Keep decoder warnings visible: corruption must not be hidden. */
    av_log_set_level(AV_LOG_WARNING);

    DVDIO d;
    memset(&d, 0, sizeof(d));

    if (posix_memalign((void **)&d.sector, DVD_SECTOR, DVD_SECTOR) != 0 ||
        !d.sector) {
        fprintf(stderr, "Could not allocate aligned DVD sector\n");
        return 1;
    }

    stage = 1;

    fprintf(stderr, "Opening DVD %s...\n", dvd);

    if (dvdnav_open(&d.nav, dvd) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "dvdnav_open failed\n");
        free(d.sector);
        return 1;
    }

    dvdnav_set_readahead_flag(d.nav, 1);

    dvdnav_menu_language_select(d.nav, "en");
    dvdnav_audio_language_select(d.nav, "en");
    dvdnav_spu_language_select(d.nav, "en");

    fprintf(stderr, "libdvdnav opened successfully\n");

    const AVInputFormat *mpeg = av_find_input_format("mpeg");

    if (!mpeg) {
        fprintf(stderr, "MPEG-PS demuxer missing\n");
        return 2;
    }

    uint8_t *avio_buf = av_malloc(AVIO_BUF_SIZE);

    AVIOContext *avio =
        avio_alloc_context(avio_buf, AVIO_BUF_SIZE, 0, &d,
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

    fprintf(stderr, "Opening MPEG-PS demuxer...\n");

    int r = avformat_open_input(&fmt, "", mpeg, NULL);

    if (r < 0) {
        char e[128];
        fferr(r, e, sizeof(e));
        fprintf(stderr, "avformat_open_input failed: %s\n", e);
        return 4;
    }

    fprintf(stderr, "MPEG-PS demuxer opened successfully\n");

    /* Map the framebuffer up front so we fail early if it isn't there. */
    int mem_fd = -1;

    Render rc;
    memset(&rc, 0, sizeof(rc));
    rc.first_genuine_pts = AV_NOPTS_VALUE;
    rc.timeline_pts = AV_NOPTS_VALUE;
    rc.assigned_pts = AV_NOPTS_VALUE;
    rc.first_display_pts = AV_NOPTS_VALUE;
    rc.last_display_pts = AV_NOPTS_VALUE;

    rc.fb = map_mister_fb(&mem_fd);

    if (!rc.fb) {
        fprintf(stderr, "\nTEST ABORTED SAFELY (DDR framebuffer "
                        "not written).\n");
        return 11;
    }

    AVPacket *pkt = av_packet_alloc();   /* demuxed PES payload packets  */
    AVPacket *ppkt = av_packet_alloc();  /* parser-assembled ES packets  */
    AVFrame *frame = av_frame_alloc();

    if (!pkt || !ppkt || !frame)
        return 6;

    AVCodecContext *vdec = NULL;
    AVCodecParserContext *parser = NULL;

    int vi = -1;

    unsigned long demux_packets = 0;
    unsigned long parser_packets = 0;

    fprintf(stderr, "\nReading DVD stream (PTS-paced, %d frames)...\n",
            TARGET_FRAMES);

    stage = 3;

    while (rc.rendered < TARGET_FRAMES && !rc.fail &&
           (r = av_read_frame(fmt, pkt)) >= 0) {

        /* MPEG-PS discovers DVD streams dynamically. */
        if (vi < 0) {

            for (unsigned i = 0; i < fmt->nb_streams; ++i) {

                AVCodecParameters *cp = fmt->streams[i]->codecpar;

                if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {

                    vi = (int)i;

                    if (cp->codec_id == AV_CODEC_ID_NONE)
                        cp->codec_id = AV_CODEC_ID_MPEG2VIDEO;

                    fprintf(stderr, "Video stream #%d discovered.\n", vi);

                    vdec = open_video_decoder(fmt->streams[vi]);

                    if (!vdec) {
                        fprintf(stderr, "Could not open video decoder\n");
                        return 7;
                    }

                    /*
                     * Explicit MPEG-2 ES parser — required architecture,
                     * proven by dvd_frame_test: reassembles PES payload
                     * chunks into whole access units before the decoder.
                     */
                    parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);

                    if (!parser) {
                        fprintf(stderr, "Could not init MPEG-2 parser\n");
                        return 7;
                    }

                    rc.tb = fmt->streams[vi]->time_base;
                    rc.avg_frame_rate = fmt->streams[vi]->avg_frame_rate;
                    rc.r_frame_rate = fmt->streams[vi]->r_frame_rate;
                    rc.first_genuine_pts = AV_NOPTS_VALUE;
                    rc.timeline_pts = AV_NOPTS_VALUE;

                    fprintf(stderr, "Video timebase: %d/%d\n",
                            rc.tb.num, rc.tb.den);
                    fprintf(stderr, "Stream avg/r frame rate: %d/%d , %d/%d\n",
                            rc.avg_frame_rate.num, rc.avg_frame_rate.den,
                            rc.r_frame_rate.num, rc.r_frame_rate.den);

                    break;
                }
            }
        }

        if (!vdec || pkt->stream_index != vi) {
            av_packet_unref(pkt);
            continue;
        }

        demux_packets++;

        stage = 5;

        /*
         * Feed the entire demuxed payload through the parser. Timestamps
         * are attached only to the first parse call for this packet.
         */
        const uint8_t *in = pkt->data;
        int in_size = pkt->size;

        int64_t in_pts = pkt->pts;
        int64_t in_dts = pkt->dts;
        int64_t in_pos = pkt->pos;

        while (in_size > 0 && rc.rendered < TARGET_FRAMES && !rc.fail) {

            uint8_t *out_data = NULL;
            int out_size = 0;

            int used = av_parser_parse2(parser, vdec,
                                        &out_data, &out_size,
                                        in, in_size,
                                        in_pts, in_dts, in_pos);

            if (used < 0)
                break;

            in += used;
            in_size -= used;

            in_pts = AV_NOPTS_VALUE;
            in_dts = AV_NOPTS_VALUE;
            in_pos = -1;

            if (out_size <= 0)
                continue;

            parser_packets++;

            ppkt->data = out_data;
            ppkt->size = out_size;
            ppkt->pts = parser->pts;
            ppkt->dts = parser->dts;

            decode_and_render(vdec, ppkt, frame, &rc);
        }

        av_packet_unref(pkt);
    }

    /*
     * EOF (or read error) before the target: flush the parser's buffered
     * access unit, then flush the decoder, so no frame is lost.
     */
    if (rc.rendered < TARGET_FRAMES && !rc.fail && vdec && parser) {

        static const uint8_t flush_buf[AV_INPUT_BUFFER_PADDING_SIZE] = {0};

        for (;;) {

            uint8_t *out_data = NULL;
            int out_size = 0;

            av_parser_parse2(parser, vdec,
                             &out_data, &out_size,
                             flush_buf, 0,
                             AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);

            if (out_size <= 0)
                break;

            parser_packets++;

            ppkt->data = out_data;
            ppkt->size = out_size;
            ppkt->pts = parser->pts;
            ppkt->dts = parser->dts;

            if (decode_and_render(vdec, ppkt, frame, &rc))
                break;
        }

        if (rc.rendered < TARGET_FRAMES && !rc.fail)
            decode_and_render(vdec, NULL, frame, &rc);
    }

    int intervals = rc.rendered > 1 ? rc.rendered - 1 : 0;

    double first_pts_s = 0.0;
    double last_pts_s = 0.0;
    double source_s = 0.0;
    int have_pts_span = (rc.first_display_pts != AV_NOPTS_VALUE &&
                         rc.last_display_pts != AV_NOPTS_VALUE &&
                         rc.tb.num > 0 && rc.tb.den > 0);

    if (have_pts_span) {
        first_pts_s = rc.first_display_pts * av_q2d(rc.tb);
        last_pts_s = rc.last_display_pts * av_q2d(rc.tb);
        source_s = last_pts_s - first_pts_s;
    }

    double startup_s = 0.0;
    double present_s = 0.0;

    if (rc.first_present_us)
        startup_s = (double)(rc.first_present_us - program_start) / 1e6;

    if (rc.first_present_us && rc.last_present_us >= rc.first_present_us)
        present_s = (double)(rc.last_present_us - rc.first_present_us) / 1e6;

    /*
     * Decode/parser during the presentation window only: startup/pre-roll
     * (including the first convert and any sleep before first present)
     * is excluded.
     */
    int64_t decode_us = 0;

    if (rc.first_present_us && rc.last_present_us >= rc.first_present_us)
        decode_us = (rc.last_present_us - rc.first_present_us)
                    - (rc.convert_us - rc.first_convert_us)
                    - (rc.wait_us - rc.wait_us_at_first);

    fprintf(stderr,
            "\n=== STREAM STATISTICS ===\n"
            "Demuxed video packets: %lu\n"
            "Parser output packets: %lu\n"
            "Frames displayed:      %d\n"
            "Genuine PTS frames:    %d\n"
            "Interpolated frames:   %d\n"
            "25 fps fallback frames:%d\n"
            "DVD NAV packets:       %lu\n"
            "DVD MPEG sectors:      %lu\n",
            demux_packets, parser_packets, rc.rendered,
            rc.genuine_pts_count, rc.interpolated_count, rc.fallback_frames,
            d.nav_packets, d.mpeg_sectors);

    fprintf(stderr, "\n=== PRESENTATION CADENCE ===\n");

    fprintf(stderr, "Startup/pre-roll:             %8.3f s\n", startup_s);

    if (rc.fps.num > 0 && rc.fps.den > 0)
        fprintf(stderr, "Detected frame rate:          %d/%d  (%.3f fps)\n",
                rc.fps.num, rc.fps.den, av_q2d(rc.fps));
    else
        fprintf(stderr, "Detected frame rate:          not available\n");

    if (rc.first_genuine_pts != AV_NOPTS_VALUE)
        fprintf(stderr,
                "First genuine PTS:            %" PRId64 "  (%.6f s)\n",
                rc.first_genuine_pts,
                rc.first_genuine_pts * av_q2d(rc.tb));
    else
        fprintf(stderr, "First genuine PTS:            not available\n");

    fprintf(stderr,
            "Max PTS vs interpol. error:   %8.3f ms\n",
            (double)rc.max_disc_us / 1000.0);

    if (rc.first_display_pts != AV_NOPTS_VALUE)
        fprintf(stderr,
                "First displayed frame PTS:    %" PRId64 "  (%.6f s)\n",
                rc.first_display_pts, first_pts_s);
    else
        fprintf(stderr, "First displayed frame PTS:    not available\n");

    if (rc.last_display_pts != AV_NOPTS_VALUE)
        fprintf(stderr,
                "Last displayed frame PTS:     %" PRId64 "  (%.6f s)\n",
                rc.last_display_pts, last_pts_s);
    else
        fprintf(stderr, "Last displayed frame PTS:     not available\n");

    fprintf(stderr,
            "Source duration (last-first): %8.3f s%s\n"
            "Source fps (%d intervals):    %8.3f\n"
            "Presentation window:          %8.3f s\n"
            "Presented fps (%d intervals): %8.3f\n"
            "Late frames (vs PTS target):  %8d\n"
            "Average lateness (late only): %8.3f ms\n"
            "Maximum lateness:             %8.3f ms\n"
            "Decode/parser (excl. sleep):  %8.3f s  (%7.3f ms/frame)\n"
            "Direct YUV -> DDR BGR0:       %8.3f s  (%7.3f ms/frame)\n"
            "Deliberate sleep time:        %8.3f s\n",
            source_s, have_pts_span ? "" : "  (no PTS span)",
            intervals,
            (have_pts_span && source_s > 0.0 && intervals > 0)
                ? (double)intervals / source_s : 0.0,
            present_s,
            intervals,
            (present_s > 0.0 && intervals > 0)
                ? (double)intervals / present_s : 0.0,
            rc.late_frames,
            rc.late_frames > 0
                ? (double)rc.late_us_sum / 1000.0 / rc.late_frames
                : 0.0,
            (double)rc.late_us_max / 1000.0,
            (double)decode_us / 1e6,
            rc.rendered > 0
                ? (double)decode_us / 1000.0 / rc.rendered : 0.0,
            (double)rc.convert_us / 1e6,
            rc.rendered > 0
                ? (double)rc.convert_us / 1000.0 / rc.rendered : 0.0,
            (double)rc.wait_us / 1e6);

    /* Cleanup (the last frame stays visible in DDR). */
    munmap(rc.fb, FB_SIZE);
    close(mem_fd);

    if (rc.sws)
        sws_freeContext(rc.sws);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_packet_free(&ppkt);
    av_parser_close(parser);
    avcodec_free_context(&vdec);
    avformat_close_input(&fmt);
    avio_context_free(&avio);
    dvdnav_close(d.nav);
    free(d.sector);

    if (rc.fail)
        return 10;

    if (rc.rendered >= TARGET_FRAMES) {
        fprintf(stderr,
                "\nPASS: %d consecutive DVD frames delivered to "
                "MISTER_FB.\n", TARGET_FRAMES);
        return 0;
    }

    fprintf(stderr, "\nFAIL: stream ended after %d frames.\n", rc.rendered);
    return 9;
}
