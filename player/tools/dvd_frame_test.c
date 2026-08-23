/*
 * dvd_frame_test.c — smallest possible proof connecting the working DVD
 * decoder to the proven MISTER_FB DDR framebuffer.
 *
 * Pipeline (one frame only):
 *   /dev/sr0 -> libdvdnav -> custom AVIO -> MPEG-PS demux -> MPEG-2 decode
 *   -> decode normally to frame CAPTURE_FRAME (so all reference frames are
 *   available and we are past any black fade-in) -> confirm 720x576 (PAL
 *   PoC) -> swscale YUV->BGR0 at native resolution (NO resizing) -> copy
 *   into DDR at 0x30000000 (the framebuffer the DVD core / ASCAL scans out,
 *   tag working-mister-fb).
 *
 * The DVD-side code (DVDIO callback, decoder setup, SAR handling, SIGILL
 * diagnostics) is copied from the proven player/src/dvdplayer_headless.c.
 * The DDR-side code (/proc/iomem System RAM check, /dev/mem mapping of
 * exactly the framebuffer region) is copied from ddr_fb_writer.c.
 *
 * Deliberately NOT implemented: continuous playback, audio, controls,
 * double buffering, NTSC, menus, synchronization.
 *
 * The frame is left in DDR on exit so it can be inspected on the TV.
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

/* Decode normally and capture this frame (1-based). Frame 1 on many discs
 * is a black fade-in, which displayed as an all-black screen. */
#define CAPTURE_FRAME 150

/* Give up if the stream ends before this many decoded frames. */
#define MAX_DECODED_FRAMES 500

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

/*
 * Send one parsed packet (or NULL to flush) to the decoder and drain
 * decoded frames. Returns 1 once the capture-target frame is reached
 * (the frame is left referenced in *frame), 0 otherwise.
 */
static int decode_and_maybe_capture(AVCodecContext *vdec,
                                    AVPacket *ppkt,
                                    AVFrame *frame,
                                    int *decoded,
                                    int *captured,
                                    int capture_frame)
{
    if (avcodec_send_packet(vdec, ppkt) < 0)
        return 0;

    while (!*captured &&
           avcodec_receive_frame(vdec, frame) == 0) {

        (*decoded)++;

        if (*decoded >= capture_frame) {
            *captured = 1;
            return 1;               /* keep this frame referenced */
        }

        av_frame_unref(frame);
    }

    return 0;
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
/* Main: decode one I-frame, convert, copy to DDR                      */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    install_sigill_handler();

    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "=== SS1 DVD -> MISTER_FB SINGLE FRAME TEST ===\n");
    fprintf(stderr, "FFmpeg CPU flags: 0x%x\n", av_get_cpu_flags());

    const char *dvd = argc > 1 ? argv[1] : "/dev/sr0";

    /*
     * Deliberately keep FFmpeg warnings visible for this test: we want to
     * see whether the "ac-tex damaged" / "MVs not available" / "invalid
     * cbp" corruption messages disappear with the explicit ES parser.
     */
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
    int decoded = 0;
    int captured = 0;

    fprintf(stderr, "\nReading DVD stream...\n"
                    "Routing video payload through the MPEG-2 ES parser,\n"
                    "capturing frame %d...\n",
            CAPTURE_FRAME);

    stage = 3;

    while (!captured &&
           decoded < MAX_DECODED_FRAMES &&
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
                     * Explicit MPEG-2 ES parser: reassembles the PES
                     * payload chunks into whole access units (one coded
                     * picture per packet) before they reach the decoder.
                     */
                    parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);

                    if (!parser) {
                        fprintf(stderr,
                                "Could not init MPEG-2 parser\n");
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
         * are attached only to the first parse call for this packet; the
         * parser assigns them to the access unit that starts there.
         */
        const uint8_t *in = pkt->data;
        int in_size = pkt->size;

        int64_t in_pts = pkt->pts;
        int64_t in_dts = pkt->dts;
        int64_t in_pos = pkt->pos;

        while (in_size > 0 && !captured) {

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

            decode_and_maybe_capture(vdec, ppkt, frame,
                                     &decoded, &captured,
                                     CAPTURE_FRAME);
        }

        av_packet_unref(pkt);
    }

    /*
     * EOF (or read error) before the target frame: flush the parser's
     * buffered access unit, then flush the decoder, so no frame is lost.
     */
    if (!captured && vdec && parser) {

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

            if (decode_and_maybe_capture(vdec, ppkt, frame,
                                         &decoded, &captured,
                                         CAPTURE_FRAME))
                break;
        }

        if (!captured)
            decode_and_maybe_capture(vdec, NULL, frame,
                                     &decoded, &captured,
                                     CAPTURE_FRAME);
    }

    fprintf(stderr,
            "\n=== STREAM STATISTICS ===\n"
            "Demuxed video packets: %lu\n"
            "Parser output packets: %lu\n"
            "Decoded frames:        %d\n",
            demux_packets, parser_packets, decoded);

    if (!captured) {
        fprintf(stderr,
                "\nFAIL: stream ended after %d decoded frames "
                "(wanted frame %d).\n",
                decoded, CAPTURE_FRAME);
        return 9;
    }

    /* ---- Report everything we know about the frame ---- */

    AVRational sar = frame_sar(frame, vdec);
    AVRational tb = fmt->streams[vi]->time_base;

    int64_t pts = frame->pts != AV_NOPTS_VALUE
                      ? frame->pts
                      : frame->best_effort_timestamp;

    fprintf(stderr, "\n=== CAPTURED FRAME ===\n");
    fprintf(stderr, "Frame number:        %d (of %d decoded)\n",
            CAPTURE_FRAME, decoded);
    fprintf(stderr, "Dimensions:          %dx%d\n",
            frame->width, frame->height);
    fprintf(stderr, "Pixel format:        %s\n",
            av_get_pix_fmt_name(frame->format)
                ? av_get_pix_fmt_name(frame->format)
                : "unknown");
    fprintf(stderr, "Sample aspect ratio: %d:%d\n", sar.num, sar.den);
    fprintf(stderr, "Frame type:          %c\n",
            av_get_picture_type_char(frame->pict_type));

    if (pts != AV_NOPTS_VALUE)
        fprintf(stderr, "PTS:                 %" PRId64
                        " (timebase %d/%d = %.3f s)\n",
                pts, tb.num, tb.den, pts * av_q2d(tb));
    else
        fprintf(stderr, "PTS:                 not available\n");

    /* ---- Confirm PAL PoC geometry: must be exactly 720x576 ---- */

    if (frame->width != FB_W || frame->height != FB_H) {
        fprintf(stderr,
                "\nFAIL: frame is %dx%d, expected exactly %dx%d for this\n"
                "PAL proof-of-concept. Refusing to write to the MISTER_FB\n"
                "buffer (no resizing allowed in this test).\n",
                frame->width, frame->height, FB_W, FB_H);
        return 10;
    }

    fprintf(stderr, "OK: frame is exactly %dx%d.\n", FB_W, FB_H);

    /* ---- Convert this ONE frame to BGR0 at native size (no resize) ---- */

    stage = 7;

    uint8_t *dst_data[4] = {0};
    int dst_linesize[4] = {0};

    if (av_image_alloc(dst_data, dst_linesize,
                       FB_W, FB_H, AV_PIX_FMT_BGR0, 32) < 0) {
        fprintf(stderr, "Could not allocate BGR0 frame\n");
        return 8;
    }

    struct SwsContext *sws =
        sws_getContext(FB_W, FB_H, frame->format,
                       FB_W, FB_H, AV_PIX_FMT_BGR0,
                       SWS_FAST_BILINEAR, NULL, NULL, NULL);

    if (!sws) {
        fprintf(stderr, "Could not create swscale context\n");
        return 8;
    }

    int64_t t0 = av_gettime_relative();

    sws_scale(sws,
              (const uint8_t * const *)frame->data, frame->linesize,
              0, FB_H,
              dst_data, dst_linesize);

    int64_t t1 = av_gettime_relative();

    double convert_ms = (double)(t1 - t0) / 1000.0;

    /*
     * ---- Image statistics of the converted BGR0 frame ----
     * Establishes whether the source image is already essentially
     * all-white/black BEFORE it reaches the MISTER_FB buffer.
     */
    {
        unsigned min_r = 255, min_g = 255, min_b = 255;
        unsigned max_r = 0,   max_g = 0,   max_b = 0;

        uint64_t sum_r = 0, sum_g = 0, sum_b = 0;

        for (int y = 0; y < FB_H; y++) {

            const uint32_t *row =
                (const uint32_t *)(dst_data[0] +
                                   (size_t)y * dst_linesize[0]);

            for (int x = 0; x < FB_W; x++) {

                uint32_t v = row[x];

                unsigned pr = (v >> 16) & 0xff;
                unsigned pg = (v >> 8) & 0xff;
                unsigned pb = v & 0xff;

                if (pr < min_r) min_r = pr;
                if (pg < min_g) min_g = pg;
                if (pb < min_b) min_b = pb;

                if (pr > max_r) max_r = pr;
                if (pg > max_g) max_g = pg;
                if (pb > max_b) max_b = pb;

                sum_r += pr;
                sum_g += pg;
                sum_b += pb;
            }
        }

        double n = (double)FB_W * (double)FB_H;

        fprintf(stderr,
                "\n=== CONVERTED IMAGE STATISTICS (BGR0) ===\n"
                "R: min %3u  max %3u  avg %6.1f\n"
                "G: min %3u  max %3u  avg %6.1f\n"
                "B: min %3u  max %3u  avg %6.1f\n",
                min_r, max_r, (double)sum_r / n,
                min_g, max_g, (double)sum_g / n,
                min_b, max_b, (double)sum_b / n);
    }

    /* ---- Map the proven MISTER_FB DDR buffer and copy the frame ---- */

    int mem_fd = -1;
    uint8_t *fb = map_mister_fb(&mem_fd);

    if (!fb) {
        fprintf(stderr, "\nTEST ABORTED SAFELY (DDR framebuffer "
                        "not written).\n");
        return 11;
    }

    t0 = av_gettime_relative();

    for (int y = 0; y < FB_H; y++)
        memcpy(fb + (size_t)y * FB_STRIDE,
               dst_data[0] + (size_t)y * dst_linesize[0],
               (size_t)FB_W * 4);

    t1 = av_gettime_relative();

    double copy_ms = (double)(t1 - t0) / 1000.0;

    fprintf(stderr,
            "\n=== TIMINGS ===\n"
            "YUV -> BGR0 conversion: %7.3f ms\n"
            "DDR framebuffer copy:   %7.3f ms  (%6.1f MB/s)\n",
            convert_ms,
            copy_ms, (FB_SIZE / (1024.0 * 1024.0)) / (copy_ms / 1000.0));

    fprintf(stderr,
            "\nThe decoded DVD frame is now in DDR at 0x%08lx.\n"
            "The DVD core / ASCAL should be displaying it. The image stays\n"
            "there after this program exits - inspect it on the TV.\n",
            FB_PHYS_BASE);

    fprintf(stderr, "DVD NAV packets: %lu\n", d.nav_packets);
    fprintf(stderr, "DVD MPEG sectors: %lu\n", d.mpeg_sectors);

    /* Cleanup (does not touch the DDR image). */
    munmap(fb, FB_SIZE);
    close(mem_fd);

    sws_freeContext(sws);
    av_freep(&dst_data[0]);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_packet_free(&ppkt);
    av_parser_close(parser);
    avcodec_free_context(&vdec);
    avformat_close_input(&fmt);
    avio_context_free(&avio);
    dvdnav_close(d.nav);
    free(d.sector);

    fprintf(stderr, "\nPASS: DVD frame delivered to MISTER_FB framebuffer.\n");
    return 0;
}
