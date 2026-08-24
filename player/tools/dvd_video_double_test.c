/*
 * dvd_video_double_test.c — PTS-paced DVD -> mailbox double-buffer test.
 *
 * Same decode path as dvd_video_test.c (do not replace that tool):
 *   /dev/sr0 -> libdvdnav -> custom AVIO -> MPEG-PS demux
 *   -> explicit MPEG-2 ES parser -> MPEG-2 decode
 *   -> swscale YUV->BGR0 at native 720x576 written DIRECTLY into the
 *      inactive MISTER_FB buffer, then a mailbox flip at 0x30400000.
 *
 * Buffer A 0x30000000, Buffer B 0x30200000, mailbox bit0 = requested FB.
 * Leave OSD Buffer on A (status[8] ORs with the mailbox in the proven RBF).
 *
 * Mailbox RTL: FPGA polls 0x30400000 ~0.82 ms and publishes FB_BASE
 * only on FB_VBL. ARM converts as soon as the dest buffer is inactive,
 * then writes the mailbox in the VBL interval before the PTS-chosen
 * HDMI presentation VBL, with a 2 ms poll margin.
 *
 * Optional CLI (does not change the decode/mailbox path):
 *   dvd_video_double_test [device] [--title N] [--chapter N]
 *   dvd_video_double_test [device] --list-titles
 * No --title keeps DVD First Play. --list-titles does not run the benchmark.
 *
 * Deliberately NOT implemented: audio, controls, menus, frame dropping,
 * extra buffers, RTL changes.
 *
 * Requires: mailbox DVD core loaded and a PAL DVD in /dev/sr0
 * (except --list-titles, which only needs the disc).
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
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#define DVD_SECTOR    2048
#define AVIO_BUF_SIZE (64 * 1024)

#define TARGET_FRAMES 500
#define FALLBACK_FPS  25

#define FB_A_PHYS     0x30000000UL
#define FB_B_PHYS     0x30200000UL
#define MB_PHYS       0x30400000UL
#define FB_W          720
#define FB_H          576
#define FB_STRIDE     2880
#define FB_SIZE       ((size_t)FB_STRIDE * FB_H)
#define MB_MAP_SIZE   4096

/* Proven mailbox: poll ~0.82 ms, publish on FB_VBL. */
#define STARTUP_DISPLAY_VBLS     2
#define MAILBOX_POLL_MARGIN_US   2000
#define VSYNC_SAMPLE_INTERVALS   30

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

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
/* DVD -> AVIO plumbing (from dvd_video_test.c)                        */
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
/* MISTER_FB A/B + mailbox                                             */
/* ------------------------------------------------------------------ */

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

        if (sscanf(line, " %llx-%llx :", &start, &end) != 2)
            continue;

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

static int check_range(unsigned long base, size_t size)
{
    fprintf(stderr, "Target range: 0x%08lx-0x%08lx (%zu bytes)\n",
            base, base + size - 1, size);

    int ov = overlaps_system_ram(base, size);

    if (ov > 0) {
        fprintf(stderr, "FAIL: 0x%08lx overlaps Linux System RAM.\n", base);
        return -1;
    }
    if (ov < 0)
        fprintf(stderr,
                "WARNING: could not verify via /proc/iomem.\n");
    else
        fprintf(stderr, "OK: no overlap with System RAM.\n");

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

static void wait_n_vsync(int fb_fd, int n, int64_t *acc_us)
{
    int64_t t0 = av_gettime_relative();

    for (int i = 0; i < n; i++)
        wait_vsync(fb_fd);

    if (acc_us)
        *acc_us += av_gettime_relative() - t0;
}

typedef struct {
    uint8_t *fb_a;
    uint8_t *fb_b;
    volatile uint32_t *mbox;
    int mem_fd;
    int fb_fd;
    int vsync_ok;
    int64_t vsync_period_us;
    int64_t vbl_origin_us;        /* timestamp of VBL index 0           */
    int64_t margin_us;            /* poll + DDR margin before publish   */
} FBPair;

static int map_double_fb(FBPair *fb)
{
    stage = 6;

    memset(fb, 0, sizeof(*fb));
    fb->mem_fd = -1;
    fb->fb_fd = -1;

    fprintf(stderr,
            "\nMapping MISTER_FB double buffers + mailbox:\n"
            "  A=0x%08lx  B=0x%08lx  mailbox=0x%08lx\n"
            "Leave OSD Buffer on A.\n",
            FB_A_PHYS, FB_B_PHYS, MB_PHYS);

    fprintf(stderr, "Checking /proc/iomem for System RAM overlap...\n");

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
        fprintf(stderr, "open /dev/fb0: %s (will sleep 20 ms)\n",
                strerror(errno));
        fb->vsync_ok = 0;
    } else if (wait_vsync(fb->fb_fd) < 0) {
        fprintf(stderr, "FBIO_WAITFORVSYNC failed; using 20 ms sleep\n");
        fb->vsync_ok = 0;
    } else {
        fb->vsync_ok = 1;
        fprintf(stderr, "Using FBIO_WAITFORVSYNC.\n");
    }

    /* Known displayed buffer = A. */
    fb->mbox[0] = 0;
    wait_n_vsync(fb->fb_fd, STARTUP_DISPLAY_VBLS, NULL);

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

    fprintf(stderr,
            "HDMI vsync: %d intervals in %.3f ms  ->  %.3f ms/period  "
            "(%.3f Hz)\n"
            "Mailbox poll margin: %.3f ms before publication VBL\n",
            VSYNC_SAMPLE_INTERVALS,
            (double)(t1 - t0) / 1000.0,
            (double)fb->vsync_period_us / 1000.0,
            1e6 / (double)fb->vsync_period_us,
            (double)fb->margin_us / 1000.0);

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

/* ------------------------------------------------------------------ */
/* Per-frame rendering into the inactive buffer + mailbox request      */
/* ------------------------------------------------------------------ */

typedef struct {
    FBPair *fb;

    struct SwsContext *sws;
    uint8_t *dst_data[4];
    int dst_linesize[4];

    int displayed;                /* 0 = A on screen, 1 = B             */
    int64_t present_vbl;          /* scheduled publication VBL, or -1   */
    int64_t first_present_vbl;
    int64_t last_present_vbl;
    int rendered;
    int frames_a;
    int frames_b;
    int flips;

    int64_t convert_us;
    int64_t wait_us;              /* dest-free + mailbox-window waits   */
    int64_t vsync_us;             /* FBIO_WAITFORVSYNC time inside waits */

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
    int64_t vsync_us_at_first;
    int64_t vsync_us_at_last;     /* vsync_us at last mailbox write     */

    int not_ready;                /* missed PTS-nearest publication VBL */
    int64_t late_us_sum;
    int64_t late_us_max;

    int fail;
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
 * Existing PTS playback clock (same rules as dvd_video_test.c).
 * Returns the PTS presentation target in av_gettime_relative() us.
 */
static int64_t assign_pts_target(Render *rc, const AVFrame *frame,
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
    return target;
}

static int64_t vbl_time(const FBPair *fb, int64_t idx)
{
    return fb->vbl_origin_us + idx * fb->vsync_period_us;
}

static int64_t vbl_nearest(const FBPair *fb, int64_t t_us)
{
    int64_t p = fb->vsync_period_us;

    if (p <= 0)
        return 0;

    if (t_us <= fb->vbl_origin_us)
        return 0;

    return (t_us - fb->vbl_origin_us + p / 2) / p;
}

static void wait_until(Render *rc, int64_t target)
{
    int64_t now = av_gettime_relative();

    if (now >= target)
        return;

    int64_t t0 = now;
    int64_t remain = target - now;

    if (remain > 2500)
        av_usleep((unsigned)(remain - 1500));

    while (av_gettime_relative() < target) {
        int64_t v0 = av_gettime_relative();
        wait_vsync(rc->fb->fb_fd);
        rc->vsync_us += av_gettime_relative() - v0;
    }

    rc->wait_us += av_gettime_relative() - t0;
}

/*
 * Publication VBL nearest the PTS, not before the previous frame's VBL,
 * and still reachable with poll margin after convert has finished.
 * A miss means the PTS-nearest edge was already too close; the frame
 * then appears one (or more) HDMI refreshes later.
 */
static int64_t choose_present_vbl(Render *rc, int64_t pts_target, int64_t now,
                                  int first)
{
    int64_t min_k = rc->present_vbl + 1;
    int64_t ideal = vbl_nearest(rc->fb, pts_target);
    int64_t k = ideal;
    int64_t margin = rc->fb->margin_us;

    if (k < min_k)
        k = min_k;

    while (vbl_time(rc->fb, k) - margin < now)
        k++;

    if (!first && k > ideal) {
        int64_t late = vbl_time(rc->fb, k) - vbl_time(rc->fb, ideal);

        rc->not_ready++;
        rc->late_us_sum += late;

        if (late > rc->late_us_max)
            rc->late_us_max = late;
    }

    return k;
}

static void render_frame(Render *rc, AVFrame *frame, AVCodecContext *vdec)
{
    if (frame->width != FB_W || frame->height != FB_H) {

        fprintf(stderr,
                "\nFAIL: frame %d is %dx%d, expected exactly %dx%d.\n"
                "No resizing allowed in this test.\n",
                rc->rendered + 1, frame->width, frame->height, FB_W, FB_H);

        rc->fail = 1;
        return;
    }

    if (!rc->sws) {

        AVRational sar = frame_sar(frame, vdec);
        AVRational fps0 = detect_fps(rc, vdec);

        fprintf(stderr,
                "\n=== VIDEO PATH STARTING (mailbox double-buffer) ===\n"
                "Dimensions:          %dx%d\n"
                "Pixel format:        %s\n"
                "Sample aspect ratio: %d:%d\n"
                "swscale dest:        inactive DDR A/B (stride %d)\n"
                "Detected frame rate: %d/%d (%.3f fps)\n"
                "HDMI vsync period:   %.3f ms  (%.3f Hz)\n"
                "Mailbox poll margin: %.3f ms\n"
                "Pacing:              PTS -> nearest HDMI VBL (2/3 cadence)\n"
                "Pushing %d paced frames...\n\n",
                frame->width, frame->height,
                av_get_pix_fmt_name(frame->format)
                    ? av_get_pix_fmt_name(frame->format)
                    : "unknown",
                sar.num, sar.den,
                FB_STRIDE,
                fps0.num, fps0.den,
                (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0,
                (double)rc->fb->vsync_period_us / 1000.0,
                1e6 / (double)rc->fb->vsync_period_us,
                (double)rc->fb->margin_us / 1000.0,
                TARGET_FRAMES);

        rc->sws = sws_getContext(FB_W, FB_H, frame->format,
                                 FB_W, FB_H, AV_PIX_FMT_BGR0,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);

        if (!rc->sws) {
            fprintf(stderr, "Could not create swscale context\n");
            rc->fail = 1;
            return;
        }
    }

    int64_t pts_target = assign_pts_target(rc, frame, vdec);

    /*
     * Decode already finished in the caller. Wait until the previous
     * frame's publication VBL so the dest buffer is inactive, then
     * convert immediately — before any mailbox/PTS sleep.
     */
    if (rc->present_vbl >= 0)
        wait_until(rc, vbl_time(rc->fb, rc->present_vbl));

    int next = rc->displayed ^ 1;
    uint8_t *dst = next ? rc->fb->fb_b : rc->fb->fb_a;

    rc->dst_data[0] = dst;
    rc->dst_linesize[0] = FB_STRIDE;

    stage = 7;

    int64_t t0 = av_gettime_relative();

    sws_scale(rc->sws,
              (const uint8_t * const *)frame->data, frame->linesize,
              0, FB_H,
              rc->dst_data, rc->dst_linesize);

    int64_t t1 = av_gettime_relative();

    stage = 5;

    rc->convert_us += t1 - t0;

    __sync_synchronize();

    int64_t now = av_gettime_relative();
    int first = (rc->rendered == 0);
    int64_t k = choose_present_vbl(rc, pts_target, now, first);

    /* Write mailbox after VBL k-1 so it is not published one VBL early,
     * and at least margin_us before VBL k so the poller has fetched it. */
    if (k > 0)
        wait_until(rc, vbl_time(rc->fb, k - 1));

    rc->fb->mbox[0] = (uint32_t)next;

    if (next)
        rc->frames_b++;
    else
        rc->frames_a++;

    rc->flips++;
    rc->present_vbl = k;
    rc->displayed = next;

    {
        int64_t present_us = vbl_time(rc->fb, k);
        int64_t display_pts = rc->assigned_pts;

        if (!rc->first_present_us) {
            rc->first_present_us = present_us;
            rc->first_present_vbl = k;
            rc->first_display_pts = display_pts;
            rc->first_convert_us = t1 - t0;
            rc->wait_us_at_first = rc->wait_us;
            rc->vsync_us_at_first = rc->vsync_us;
        }

        if (rc->first_display_pts == AV_NOPTS_VALUE &&
            display_pts != AV_NOPTS_VALUE)
            rc->first_display_pts = display_pts;

        rc->last_present_us = present_us;
        rc->last_present_vbl = k;
        rc->last_display_pts = display_pts;
        rc->vsync_us_at_last = rc->vsync_us;
    }

    rc->rendered++;

    if ((rc->rendered % 100) == 0)
        fprintf(stderr,
                "Displayed %d/%d frames  (A=%d B=%d flips=%d vbl=%" PRId64 ")\n",
                rc->rendered, TARGET_FRAMES,
                rc->frames_a, rc->frames_b, rc->flips, k);
}

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
/* CLI: optional --title / --chapter / --list-titles                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *device;
    int title;          /* 0 = First Play (no jump) */
    int chapter;        /* 0 = start of title (part 1 via title_play) */
    int list_titles;
} Cli;

static void usage(void)
{
    fprintf(stderr,
            "Usage: dvd_video_double_test [device] [--title N] [--chapter N]\n"
            "       dvd_video_double_test [device] --list-titles\n"
            "device defaults to /dev/sr0.\n"
            "--chapter requires --title. No --title keeps First Play.\n");
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

        if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage();
            return -1;
        }

        cli->device = argv[i];
    }

    if (cli->list_titles && (cli->title || cli->chapter)) {
        fprintf(stderr, "--list-titles cannot be combined with --title/--chapter\n");
        return -1;
    }

    if (cli->chapter && !cli->title) {
        fprintf(stderr, "--chapter requires --title\n");
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
            fprintf(stderr, "  Title %2d:  parts unavailable (%s)\n",
                    (int)t, dvdnav_err_to_string(nav));
        else
            fprintf(stderr, "  Title %2d:  %d chapter(s)\n",
                    (int)t, (int)parts);
    }

    return 0;
}

static int jump_to_title(dvdnav_t *nav, const Cli *cli, int32_t ntitles,
                         int32_t *parts_out)
{
    int32_t parts = -1;

    if (cli->title < 1 || cli->title > ntitles) {
        fprintf(stderr, "Title %d is out of range (1..%d)\n",
                cli->title, (int)ntitles);
        return -1;
    }

    if (dvdnav_get_number_of_parts(nav, cli->title, &parts) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "Could not read chapter count for title %d: %s\n",
                cli->title, dvdnav_err_to_string(nav));
        parts = -1;
    }

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

    if (parts_out)
        *parts_out = parts;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    install_sigill_handler();

    setvbuf(stderr, NULL, _IONBF, 0);

    Cli cli;

    if (parse_cli(argc, argv, &cli) < 0)
        return 1;

    if (cli.list_titles) {

        dvdnav_t *nav = NULL;

        fprintf(stderr, "=== DVD TITLE LIST ===\n");
        fprintf(stderr, "Device: %s\n", cli.device);

        if (dvdnav_open(&nav, cli.device) != DVDNAV_STATUS_OK) {
            fprintf(stderr, "dvdnav_open failed\n");
            return 1;
        }

        int lr = list_dvd_titles(nav);
        dvdnav_close(nav);
        return lr < 0 ? 1 : 0;
    }

    const int64_t program_start = av_gettime_relative();

    fprintf(stderr, "=== SS1 DVD -> MAILBOX DOUBLE-BUFFER VIDEO TEST ===\n");
    fprintf(stderr, "FFmpeg CPU flags: 0x%x\n", av_get_cpu_flags());
    fprintf(stderr, "Leave OSD Buffer on A.\n");

    const char *dvd = cli.device;

    av_log_set_level(AV_LOG_WARNING);

    /*
     * Mailbox + HDMI vsync measurement must finish BEFORE any DVD/MPEG
     * I/O. The proven dvd_video_test.c maps the FB after avformat_open_input
     * but that map does not block. Doing ~30 vsync waits here used to sit
     * between open_input and the read loop, which left the MPEG-PS demuxer
     * with one buffered packet and then EOF on the next av_read_frame.
     */
    FBPair fb;

    if (map_double_fb(&fb) < 0) {
        fprintf(stderr, "\nTEST ABORTED SAFELY (DDR framebuffer "
                        "not written).\n");
        return 11;
    }

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

    int32_t ntitles = 0;
    int32_t nparts = -1;

    if (dvdnav_get_number_of_titles(d.nav, &ntitles) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "dvdnav_get_number_of_titles failed: %s\n",
                dvdnav_err_to_string(d.nav));
        ntitles = 0;
    }

    fprintf(stderr, "Titles on disc: %d\n", (int)ntitles);

    if (cli.title) {
        if (jump_to_title(d.nav, &cli, ntitles, &nparts) < 0)
            return 1;
    }

    if (cli.title)
        fprintf(stderr, "Selected title:   %d\n", cli.title);
    else
        fprintf(stderr, "Selected title:   First Play (default)\n");

    if (cli.chapter)
        fprintf(stderr, "Selected chapter: %d\n", cli.chapter);
    else if (cli.title)
        fprintf(stderr, "Selected chapter: 1 (title start)\n");
    else
        fprintf(stderr, "Selected chapter: n/a (First Play)\n");

    if (cli.title && nparts >= 0)
        fprintf(stderr, "Chapters in title %d: %d\n", cli.title, (int)nparts);
    else if (cli.title)
        fprintf(stderr, "Chapters in title %d: unavailable\n", cli.title);
    else
        fprintf(stderr, "Chapters in selected title: n/a (First Play)\n");

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

    Render rc;
    memset(&rc, 0, sizeof(rc));
    rc.fb = &fb;
    rc.displayed = 0;
    rc.present_vbl = -1;
    rc.first_genuine_pts = AV_NOPTS_VALUE;
    rc.timeline_pts = AV_NOPTS_VALUE;
    rc.assigned_pts = AV_NOPTS_VALUE;
    rc.first_display_pts = AV_NOPTS_VALUE;
    rc.last_display_pts = AV_NOPTS_VALUE;

    AVPacket *pkt = av_packet_alloc();
    AVPacket *ppkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    if (!pkt || !ppkt || !frame)
        return 6;

    AVCodecContext *vdec = NULL;
    AVCodecParserContext *parser = NULL;

    int vi = -1;

    unsigned long demux_packets = 0;
    unsigned long parser_packets = 0;

    fprintf(stderr, "\nReading DVD stream (mailbox-paced, %d frames)...\n",
            TARGET_FRAMES);

    stage = 3;

    int read_ret = 0;

    while (rc.rendered < TARGET_FRAMES && !rc.fail &&
           (read_ret = av_read_frame(fmt, pkt)) >= 0) {

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

    if (rc.rendered < TARGET_FRAMES && !rc.fail) {
        char e[128];

        if (read_ret < 0)
            fferr(read_ret, e, sizeof(e));
        else
            snprintf(e, sizeof(e), "no av_read_frame error");

        fprintf(stderr,
                "\nOuter demux loop exited after %d frame(s): "
                "av_read_frame returned %d (%s), fail=%d, "
                "vdec=%s, parser=%s\n",
                rc.rendered, read_ret, e, rc.fail,
                vdec ? "open" : "null",
                parser ? "open" : "null");
    }

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

    int64_t decode_us = 0;

    if (rc.rendered > 1 && rc.first_present_us &&
        rc.last_present_us > rc.first_present_us) {

        decode_us = (rc.last_present_us - rc.first_present_us)
                    - (rc.convert_us - rc.first_convert_us)
                    - (rc.wait_us - rc.wait_us_at_first)
                    - (rc.vsync_us_at_last - rc.vsync_us_at_first);

        if (decode_us < 0)
            decode_us = 0;
    }

    fprintf(stderr,
            "\n=== STREAM STATISTICS ===\n"
            "Demuxed video packets: %lu\n"
            "Parser output packets: %lu\n"
            "Frames displayed:      %d\n"
            "Frames sent to A:      %d\n"
            "Frames sent to B:      %d\n"
            "Mailbox flips:         %d\n"
            "Genuine PTS frames:    %d\n"
            "Interpolated frames:   %d\n"
            "25 fps fallback frames:%d\n"
            "DVD NAV packets:       %lu\n"
            "DVD MPEG sectors:      %lu\n",
            demux_packets, parser_packets, rc.rendered,
            rc.frames_a, rc.frames_b, rc.flips,
            rc.genuine_pts_count, rc.interpolated_count, rc.fallback_frames,
            d.nav_packets, d.mpeg_sectors);

    fprintf(stderr, "\n=== PRESENTATION CADENCE ===\n");

    fprintf(stderr, "Startup/pre-roll:             %8.3f s\n", startup_s);

    if (rc.fps.num > 0 && rc.fps.den > 0)
        fprintf(stderr, "Detected DVD frame rate:      %d/%d  (%.3f fps)\n",
                rc.fps.num, rc.fps.den, av_q2d(rc.fps));
    else
        fprintf(stderr, "Detected DVD frame rate:      not available\n");

    fprintf(stderr,
            "Measured HDMI vsync period:   %8.3f ms  (%.3f Hz)\n"
            "Mailbox poll margin:          %8.3f ms\n"
            "Scheduled present VBL:        %" PRId64 " .. %" PRId64
            "  (mean %.2f VBL/frame)\n"
            "Vsync source:                 %s\n",
            (double)fb.vsync_period_us / 1000.0,
            1e6 / (double)fb.vsync_period_us,
            (double)fb.margin_us / 1000.0,
            rc.first_present_vbl, rc.last_present_vbl,
            (intervals > 0 && rc.last_present_vbl > rc.first_present_vbl)
                ? (double)(rc.last_present_vbl - rc.first_present_vbl)
                  / (double)intervals
                : 0.0,
            fb.vsync_ok ? "FBIO_WAITFORVSYNC" : "20 ms sleep fallback");

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
            "Not ready by flip deadline:   %8d\n"
            "Average request lateness:     %8.3f ms\n"
            "Maximum request lateness:     %8.3f ms\n"
            "Decode/parser (excl. wait):   %8.3f s  (%7.3f ms/frame)\n"
            "YUV -> inactive DDR BGR0:     %8.3f s  (%7.3f ms/frame)\n"
            "Schedule wait time:           %8.3f s\n"
            "HDMI vsync wait (subset):     %8.3f s\n"
            "Total waiting/vsync time:     %8.3f s\n",
            source_s, have_pts_span ? "" : "  (no PTS span)",
            intervals,
            (have_pts_span && source_s > 0.0 && intervals > 0)
                ? (double)intervals / source_s : 0.0,
            present_s,
            intervals,
            (present_s > 0.0 && intervals > 0)
                ? (double)intervals / present_s : 0.0,
            rc.not_ready,
            rc.not_ready > 0
                ? (double)rc.late_us_sum / 1000.0 / rc.not_ready
                : 0.0,
            (double)rc.late_us_max / 1000.0,
            (double)decode_us / 1e6,
            rc.rendered > 0
                ? (double)decode_us / 1000.0 / rc.rendered : 0.0,
            (double)rc.convert_us / 1e6,
            rc.rendered > 0
                ? (double)rc.convert_us / 1000.0 / rc.rendered : 0.0,
            (double)rc.wait_us / 1e6,
            (double)rc.vsync_us / 1e6,
            (double)rc.wait_us / 1e6);

    unmap_double_fb(&fb);

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
                "\nPASS: %d consecutive DVD frames delivered through "
                "mailbox A/B.\n", TARGET_FRAMES);
        return 0;
    }

    fprintf(stderr, "\nFAIL: stream ended after %d frames.\n", rc.rendered);
    return 9;
}
