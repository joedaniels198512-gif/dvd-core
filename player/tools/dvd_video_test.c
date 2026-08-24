/*
 * dvd_video_test.c — continuous-video throughput test for the complete
 * DVD -> MISTER_FB path, based on the proven dvd_frame_test.c.
 *
 * Pipeline (500 consecutive frames, UNPACED — as fast as possible):
 *   /dev/sr0 -> libdvdnav -> custom AVIO -> MPEG-PS demux
 *   -> explicit MPEG-2 ES parser (required architecture: reassembles PES
 *      payload chunks into whole access units; without it the decoder sees
 *      broken picture boundaries and produces corrupted output)
 *   -> MPEG-2 decode -> swscale YUV->BGR0 at native 720x576 (NO resizing)
 *   -> row copy into DDR at 0x30000000 (the framebuffer the DVD core /
 *      ASCAL scans out, tag working-mister-fb).
 *
 * Purpose: establish the TRUE maximum end-to-end throughput of the whole
 * working video path before any optimisation. Reports accumulated timings
 * for demux/parse/decode, YUV->BGR0 conversion, DDR write, total wall
 * time, and achieved frames/second.
 *
 * Single buffer, no vsync: tearing during the run is expected and fine.
 *
 * Deliberately NOT implemented: pacing, audio, controls, menus, double
 * buffering, NTSC, synchronization, optimisation.
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
/* Per-frame rendering: convert + DDR write, with accumulated timings  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *fb;                  /* mapped MISTER_FB DDR buffer        */

    struct SwsContext *sws;
    uint8_t *dst_data[4];
    int dst_linesize[4];

    int rendered;

    int64_t convert_us;
    int64_t copy_us;

    int fail;                     /* geometry/setup violation           */
} Render;

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

        fprintf(stderr,
                "\n=== VIDEO PATH STARTING ===\n"
                "Dimensions:          %dx%d\n"
                "Pixel format:        %s\n"
                "Sample aspect ratio: %d:%d\n"
                "Pushing %d frames unpaced (tearing expected)...\n\n",
                frame->width, frame->height,
                av_get_pix_fmt_name(frame->format)
                    ? av_get_pix_fmt_name(frame->format)
                    : "unknown",
                sar.num, sar.den,
                TARGET_FRAMES);

        if (av_image_alloc(rc->dst_data, rc->dst_linesize,
                           FB_W, FB_H, AV_PIX_FMT_BGR0, 32) < 0) {
            fprintf(stderr, "Could not allocate BGR0 frame\n");
            rc->fail = 1;
            return;
        }

        rc->sws = sws_getContext(FB_W, FB_H, frame->format,
                                 FB_W, FB_H, AV_PIX_FMT_BGR0,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);

        if (!rc->sws) {
            fprintf(stderr, "Could not create swscale context\n");
            rc->fail = 1;
            return;
        }
    }

    stage = 7;

    int64_t t0 = av_gettime_relative();

    sws_scale(rc->sws,
              (const uint8_t * const *)frame->data, frame->linesize,
              0, FB_H,
              rc->dst_data, rc->dst_linesize);

    int64_t t1 = av_gettime_relative();

    for (int y = 0; y < FB_H; y++)
        memcpy(rc->fb + (size_t)y * FB_STRIDE,
               rc->dst_data[0] + (size_t)y * rc->dst_linesize[0],
               (size_t)FB_W * 4);

    int64_t t2 = av_gettime_relative();

    stage = 5;

    rc->convert_us += t1 - t0;
    rc->copy_us += t2 - t1;

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

    fprintf(stderr, "=== SS1 DVD -> MISTER_FB CONTINUOUS VIDEO TEST ===\n");
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

    fprintf(stderr, "\nReading DVD stream (unpaced, %d frames)...\n",
            TARGET_FRAMES);

    stage = 3;

    int64_t wall_start = av_gettime_relative();

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

    int64_t wall_us = av_gettime_relative() - wall_start;

    /* Everything not spent converting/copying is demux+parse+decode. */
    int64_t decode_us = wall_us - rc.convert_us - rc.copy_us;

    double wall_s = (double)wall_us / 1e6;

    double per_frame = rc.rendered > 0 ? 1.0 / rc.rendered : 0.0;

    fprintf(stderr,
            "\n=== STREAM STATISTICS ===\n"
            "Demuxed video packets: %lu\n"
            "Parser output packets: %lu\n"
            "Frames displayed:      %d\n"
            "DVD NAV packets:       %lu\n"
            "DVD MPEG sectors:      %lu\n",
            demux_packets, parser_packets, rc.rendered,
            d.nav_packets, d.mpeg_sectors);

    fprintf(stderr,
            "\n=== THROUGHPUT RESULTS (unpaced) ===\n"
            "Total wall time:        %8.3f s\n"
            "Demux/parse/decode:     %8.3f s  (%7.3f ms/frame)\n"
            "YUV -> BGR0 convert:    %8.3f s  (%7.3f ms/frame)\n"
            "DDR framebuffer write:  %8.3f s  (%7.3f ms/frame)\n"
            "End-to-end throughput:  %8.2f fps\n",
            wall_s,
            (double)decode_us / 1e6,
            (double)decode_us / 1e3 * per_frame,
            (double)rc.convert_us / 1e6,
            (double)rc.convert_us / 1e3 * per_frame,
            (double)rc.copy_us / 1e6,
            (double)rc.copy_us / 1e3 * per_frame,
            rc.rendered > 0 ? (double)rc.rendered / wall_s : 0.0);

    /* Cleanup (the last frame stays visible in DDR). */
    munmap(rc.fb, FB_SIZE);
    close(mem_fd);

    if (rc.sws)
        sws_freeContext(rc.sws);

    if (rc.dst_data[0])
        av_freep(&rc.dst_data[0]);

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
