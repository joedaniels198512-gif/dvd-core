#define _GNU_SOURCE

#include <dvdnav/dvdnav.h>
#include <dvdnav/dvdnav_events.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/cpu.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include <linux/fb.h>

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#define DVD_SECTOR 2048
#define AVIO_BUF_SIZE (64 * 1024)
#define TARGET_VIDEO_FRAMES 500

static volatile sig_atomic_t stage = 0;

static const char *stage_names[] = {
    "startup",
    "dvdnav",
    "mpeg demux",
    "packet read",
    "video decoder",
    "frame decode",
    "framebuffer",
    "swscale/render"
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

    dprintf(STDERR_FILENO,
            "\n*** SIGILL / illegal instruction ***\n");

    dprintf(STDERR_FILENO,
            "Stage %d: %s\n",
            (int)stage,
            (stage >= 0 &&
             stage < (int)(sizeof(stage_names) / sizeof(stage_names[0])))
                ? stage_names[stage]
                : "unknown");

    dprintf(STDERR_FILENO,
            "PC=0x%08lx LR=0x%08lx fault_addr=%p\n",
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

typedef struct {
    dvdnav_t *nav;

    uint8_t *sector;
    int sector_len;
    int sector_pos;
    int stopped;

    unsigned long nav_packets;
    unsigned long mpeg_sectors;
} DVDIO;

typedef struct {
    int fd;

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;

    uint8_t *mem;
    size_t mem_len;

    uint8_t *saved;
} FrameBuffer;

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

            memcpy(buf + out,
                   d->sector + d->sector_pos,
                   (size_t)take);

            d->sector_pos += take;
            out += take;

            continue;
        }

        int32_t event = 0;
        int32_t len = 0;

        dvdnav_status_t st =
            dvdnav_get_next_block(d->nav,
                                  d->sector,
                                  &event,
                                  &len);

        if (st != DVDNAV_STATUS_OK) {

            fprintf(stderr,
                    "dvdnav error: %s\n",
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

    const AVCodec *codec =
        avcodec_find_decoder(cp->codec_id);

    if (!codec) {
        fprintf(stderr,
                "MPEG-2 decoder unavailable\n");
        return NULL;
    }

    AVCodecContext *ctx =
        avcodec_alloc_context3(codec);

    if (!ctx)
        return NULL;

    if (avcodec_parameters_to_context(ctx, cp) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }

    ctx->pkt_timebase = st->time_base;

    int r = avcodec_open2(ctx, codec, NULL);

    fprintf(stderr,
            "avcodec_open2(%s) returned %d\n",
            codec->name,
            r);

    if (r < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }

    return ctx;
}

static int framebuffer_open(FrameBuffer *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    stage = 6;

    fb->fd = open("/dev/fb0", O_RDWR);

    if (fb->fd < 0) {
        perror("open /dev/fb0");
        return -1;
    }

    if (ioctl(fb->fd,
              FBIOGET_FSCREENINFO,
              &fb->finfo) < 0) {

        perror("FBIOGET_FSCREENINFO");
        return -1;
    }

    if (ioctl(fb->fd,
              FBIOGET_VSCREENINFO,
              &fb->vinfo) < 0) {

        perror("FBIOGET_VSCREENINFO");
        return -1;
    }

    fprintf(stderr,
            "\nFramebuffer: %ux%u, %u bpp, stride=%u\n",
            fb->vinfo.xres,
            fb->vinfo.yres,
            fb->vinfo.bits_per_pixel,
            fb->finfo.line_length);

    fprintf(stderr,
            "RGB offsets: R=%u G=%u B=%u\n",
            fb->vinfo.red.offset,
            fb->vinfo.green.offset,
            fb->vinfo.blue.offset);

    /*
     * This is the exact 32-bit BGR0 framebuffer layout
     * previously proven on the SS1.
     *
     * We deliberately refuse to write if the active Linux
     * framebuffer is something different.
     */
    if (fb->vinfo.bits_per_pixel != 32 ||
        fb->vinfo.red.offset   != 16 ||
        fb->vinfo.green.offset != 8 ||
        fb->vinfo.blue.offset  != 0) {

        fprintf(stderr,
                "Framebuffer format is not expected BGR0/XRGB8888.\n"
                "Refusing to write to it.\n");

        return -1;
    }

    fb->mem_len = fb->finfo.smem_len;

    fb->mem = mmap(NULL,
                   fb->mem_len,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED,
                   fb->fd,
                   0);

    if (fb->mem == MAP_FAILED) {
        fb->mem = NULL;
        perror("mmap framebuffer");
        return -1;
    }

    size_t last =
        ((size_t)fb->vinfo.yoffset +
         fb->vinfo.yres - 1) *
        fb->finfo.line_length +

        ((size_t)fb->vinfo.xoffset +
         fb->vinfo.xres) * 4;

    if (last > fb->mem_len) {

        fprintf(stderr,
                "Framebuffer geometry exceeds mapped memory.\n"
                "Refusing to write.\n");

        return -1;
    }

    /*
     * Save the current Linux framebuffer so we can restore
     * the screen when the test finishes.
     */
    size_t save_size =
        (size_t)fb->finfo.line_length *
        fb->vinfo.yres;

    fb->saved = malloc(save_size);

    if (!fb->saved) {
        fprintf(stderr,
                "Could not allocate framebuffer backup\n");
        return -1;
    }

    for (unsigned y = 0;
         y < fb->vinfo.yres;
         ++y) {

        uint8_t *src =
            fb->mem +
            ((size_t)fb->vinfo.yoffset + y) *
            fb->finfo.line_length;

        memcpy(fb->saved +
                   (size_t)y *
                   fb->finfo.line_length,
               src,
               fb->finfo.line_length);
    }

    fprintf(stderr,
            "Framebuffer opened safely. No video mode changes made.\n");

    return 0;
}

static void framebuffer_clear(FrameBuffer *fb)
{
    for (unsigned y = 0;
         y < fb->vinfo.yres;
         ++y) {

        uint8_t *dst =
            fb->mem +
            ((size_t)fb->vinfo.yoffset + y) *
            fb->finfo.line_length +

            (size_t)fb->vinfo.xoffset * 4;

        memset(dst, 0,
               (size_t)fb->vinfo.xres * 4);
    }
}

static void framebuffer_blit(FrameBuffer *fb,
                             uint8_t *src,
                             int src_stride,
                             int x,
                             int y,
                             int w,
                             int h)
{
    if (x < 0 || y < 0 ||
        x + w > (int)fb->vinfo.xres ||
        y + h > (int)fb->vinfo.yres) {

        return;
    }

    for (int row = 0; row < h; ++row) {

        uint8_t *dst =
            fb->mem +

            ((size_t)fb->vinfo.yoffset +
             (size_t)y +
             (size_t)row) *
            fb->finfo.line_length +

            ((size_t)fb->vinfo.xoffset +
             (size_t)x) * 4;

        memcpy(dst,
               src + (size_t)row * src_stride,
               (size_t)w * 4);
    }
}

static void framebuffer_restore(FrameBuffer *fb)
{
    if (!fb->mem || !fb->saved)
        return;

    for (unsigned y = 0;
         y < fb->vinfo.yres;
         ++y) {

        uint8_t *dst =
            fb->mem +
            ((size_t)fb->vinfo.yoffset + y) *
            fb->finfo.line_length;

        memcpy(dst,
               fb->saved +
                   (size_t)y *
                   fb->finfo.line_length,
               fb->finfo.line_length);
    }
}

static void framebuffer_close(FrameBuffer *fb)
{
    framebuffer_restore(fb);

    free(fb->saved);
    fb->saved = NULL;

    if (fb->mem)
        munmap(fb->mem, fb->mem_len);

    fb->mem = NULL;

    if (fb->fd >= 0)
        close(fb->fd);

    fb->fd = -1;
}

static AVRational frame_sar(AVFrame *frame,
                            AVCodecContext *ctx)
{
    AVRational sar = frame->sample_aspect_ratio;

    if (sar.num <= 0 || sar.den <= 0)
        sar = ctx->sample_aspect_ratio;

    if (sar.num <= 0 || sar.den <= 0)
        sar = (AVRational){1, 1};

    return sar;
}

static void calculate_output_rect(FrameBuffer *fb,
                                  AVFrame *frame,
                                  AVCodecContext *ctx,
                                  int *out_x,
                                  int *out_y,
                                  int *out_w,
                                  int *out_h,
                                  double *dar_out)
{
    AVRational sar = frame_sar(frame, ctx);

    double dar =
        ((double)frame->width *
         (double)sar.num) /
        ((double)frame->height *
         (double)sar.den);

    if (dar <= 0.1 || dar > 4.0)
        dar =
            (double)frame->width /
            (double)frame->height;

    double fb_dar =
        (double)fb->vinfo.xres /
        (double)fb->vinfo.yres;

    int w;
    int h;

    if (dar >= fb_dar) {

        w = fb->vinfo.xres;
        h = (int)lrint((double)w / dar);

    } else {

        h = fb->vinfo.yres;
        w = (int)lrint((double)h * dar);
    }

    if (w < 2) w = 2;
    if (h < 2) h = 2;

    /*
     * Even dimensions are friendlier to video scalers.
     */
    w &= ~1;
    h &= ~1;

    *out_w = w;
    *out_h = h;

    *out_x =
        ((int)fb->vinfo.xres - w) / 2;

    *out_y =
        ((int)fb->vinfo.yres - h) / 2;

    *dar_out = dar;
}

int main(int argc, char **argv)
{
    install_sigill_handler();

    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr,
            "=== SS1 NATIVE DVD VIDEO TEST ===\n");

    fprintf(stderr,
            "FFmpeg CPU flags: 0x%x\n",
            av_get_cpu_flags());

    const char *dvd =
        argc > 1 ? argv[1] : "/dev/sr0";

    av_log_set_level(AV_LOG_ERROR);

    DVDIO d;
    memset(&d, 0, sizeof(d));

    if (posix_memalign((void **)&d.sector,
                       DVD_SECTOR,
                       DVD_SECTOR) != 0 ||
        !d.sector) {

        fprintf(stderr,
                "Could not allocate aligned DVD sector\n");

        return 1;
    }

    stage = 1;

    fprintf(stderr,
            "Opening DVD %s...\n",
            dvd);

    if (dvdnav_open(&d.nav, dvd) !=
        DVDNAV_STATUS_OK) {

        fprintf(stderr,
                "dvdnav_open failed\n");

        free(d.sector);
        return 1;
    }

    dvdnav_set_readahead_flag(d.nav, 1);

    dvdnav_menu_language_select(d.nav, "en");
    dvdnav_audio_language_select(d.nav, "en");
    dvdnav_spu_language_select(d.nav, "en");

    fprintf(stderr,
            "libdvdnav opened successfully\n");

    const AVInputFormat *mpeg =
        av_find_input_format("mpeg");

    if (!mpeg) {

        fprintf(stderr,
                "MPEG-PS demuxer missing\n");

        return 2;
    }

    uint8_t *avio_buf =
        av_malloc(AVIO_BUF_SIZE);

    AVIOContext *avio =
        avio_alloc_context(avio_buf,
                           AVIO_BUF_SIZE,
                           0,
                           &d,
                           dvd_read_packet,
                           NULL,
                           NULL);

    if (!avio)
        return 3;

    avio->seekable = 0;

    AVFormatContext *fmt =
        avformat_alloc_context();

    if (!fmt)
        return 3;

    fmt->pb = avio;

    fmt->flags |=
        AVFMT_FLAG_CUSTOM_IO |
        AVFMT_FLAG_GENPTS;

    fmt->ctx_flags |=
        AVFMTCTX_UNSEEKABLE;

    stage = 2;

    fprintf(stderr,
            "Opening MPEG-PS demuxer...\n");

    int r =
        avformat_open_input(&fmt,
                            "",
                            mpeg,
                            NULL);

    if (r < 0) {

        char e[128];

        fferr(r, e, sizeof(e));

        fprintf(stderr,
                "avformat_open_input failed: %s\n",
                e);

        return 4;
    }

    fprintf(stderr,
            "MPEG-PS demuxer opened successfully\n");

    FrameBuffer fb;

    if (framebuffer_open(&fb) < 0) {

        fprintf(stderr,
                "\nVIDEO TEST ABORTED SAFELY.\n");

        return 5;
    }

    AVPacket *pkt =
        av_packet_alloc();

    AVFrame *frame =
        av_frame_alloc();

    if (!pkt || !frame) {

        framebuffer_close(&fb);
        return 6;
    }

    AVCodecContext *vdec = NULL;

    int vi = -1;

    int rendered = 0;
    int warmup = 0;

    int current_w = 0;
    int current_h = 0;
    int current_format = -1;

    int out_x = 0;
    int out_y = 0;
    int out_w = 0;
    int out_h = 0;

    int previous_out_x = -1;
    int previous_out_y = -1;
    int previous_out_w = -1;
    int previous_out_h = -1;

    uint8_t *dst_data[4] = {0};
    int dst_linesize[4] = {0};

    struct SwsContext *sws = NULL;

    double fps = 25.0;
    int64_t frame_period_us =
        (int64_t)(1000000.0 / fps);

    int64_t next_frame_us = 0;

    int64_t scale_cpu_us = 0;
    int64_t blit_cpu_us = 0;
    int late_frames = 0;

    fprintf(stderr,
            "\nReading DVD stream...\n"
            "Waiting for first MPEG-2 I-frame...\n");

    stage = 3;

    while (rendered < TARGET_VIDEO_FRAMES &&
           (r = av_read_frame(fmt, pkt)) >= 0) {

        /*
         * MPEG-PS discovers DVD streams dynamically.
         */
        if (vi < 0) {

            for (unsigned i = 0;
                 i < fmt->nb_streams;
                 ++i) {

                AVCodecParameters *cp =
                    fmt->streams[i]->codecpar;

                if (cp->codec_type ==
                    AVMEDIA_TYPE_VIDEO) {

                    vi = (int)i;

                    if (cp->codec_id ==
                        AV_CODEC_ID_NONE) {

                        cp->codec_id =
                            AV_CODEC_ID_MPEG2VIDEO;
                    }

                    fprintf(stderr,
                            "Video stream #%d discovered.\n",
                            vi);

                    vdec =
                        open_video_decoder(
                            fmt->streams[vi]);

                    if (!vdec) {

                        fprintf(stderr,
                                "Could not open video decoder\n");

                        framebuffer_close(&fb);
                        return 7;
                    }

                    break;
                }
            }
        }

        if (!vdec ||
            pkt->stream_index != vi) {

            av_packet_unref(pkt);
            continue;
        }

        stage = 5;

        if (avcodec_send_packet(vdec, pkt) < 0) {

            av_packet_unref(pkt);
            continue;
        }

        av_packet_unref(pkt);

        while (avcodec_receive_frame(vdec,
                                     frame) == 0) {

            if (rendered == 0 &&
                frame->pict_type !=
                    AV_PICTURE_TYPE_I) {

                warmup++;
                av_frame_unref(frame);
                continue;
            }

            AVRational sar =
                frame_sar(frame, vdec);

            double dar = 0.0;

            calculate_output_rect(&fb,
                                  frame,
                                  vdec,
                                  &out_x,
                                  &out_y,
                                  &out_w,
                                  &out_h,
                                  &dar);

            /*
             * BENCHMARK MODE:
             * convert YUV -> BGR0 at native DVD resolution only.
             * NO resizing and NO framebuffer copy.
             */
            out_w = frame->width;
            out_h = frame->height;
            out_x = 0;
            out_y = 0;

            /*
             * Recreate scaler/output buffer only when
             * geometry or source format changes.
             */
            if (frame->width != current_w ||
                frame->height != current_h ||
                frame->format != current_format ||
                out_w != previous_out_w ||
                out_h != previous_out_h) {

                current_w = frame->width;
                current_h = frame->height;
                current_format = frame->format;

                previous_out_w = out_w;
                previous_out_h = out_h;

                if (dst_data[0]) {
                    av_freep(&dst_data[0]);
                    memset(dst_data, 0,
                           sizeof(dst_data));
                }

                if (av_image_alloc(dst_data,
                                   dst_linesize,
                                   out_w,
                                   out_h,
                                   AV_PIX_FMT_BGR0,
                                   32) < 0) {

                    fprintf(stderr,
                            "Could not allocate RGB frame\n");

                    framebuffer_close(&fb);
                    return 8;
                }

                sws =
                    sws_getCachedContext(
                        sws,

                        frame->width,
                        frame->height,
                        frame->format,

                        out_w,
                        out_h,
                        AV_PIX_FMT_BGR0,

                        SWS_FAST_BILINEAR,

                        NULL,
                        NULL,
                        NULL);

                if (!sws) {

                    fprintf(stderr,
                            "Could not create swscale context\n");

                    framebuffer_close(&fb);
                    return 8;
                }
            }

            /*
             * If the aspect ratio rectangle changes,
             * clear the old picture so pillar/letterbox
             * areas stay black.
             */
            if (out_x != previous_out_x ||
                out_y != previous_out_y) {

                framebuffer_clear(&fb);

                previous_out_x = out_x;
                previous_out_y = out_y;
            }

            if (rendered == 0) {

                /*
                 * For this PAL test disc we use 25 fps.
                 * Later this becomes proper DVD PTS
                 * scheduling for PAL and NTSC.
                 */
                if (vdec->framerate.num > 0 &&
                    vdec->framerate.den > 0) {

                    double reported =
                        av_q2d(vdec->framerate);

                    if (reported >= 20.0 &&
                        reported <= 31.0) {

                        fps = reported;
                    }
                }

                frame_period_us =
                    (int64_t)(1000000.0 / fps);

                next_frame_us =
                    av_gettime_relative();

                fprintf(stderr,
                        "\n=== VIDEO OUTPUT STARTING ===\n");

                fprintf(stderr,
                        "DVD frame: %dx%d\n",
                        frame->width,
                        frame->height);

                fprintf(stderr,
                        "Pixel aspect ratio: %d:%d\n",
                        sar.num,
                        sar.den);

                fprintf(stderr,
                        "Display aspect ratio: %.4f\n",
                        dar);

                fprintf(stderr,
                        "Output rectangle: %dx%d at %d,%d\n",
                        out_w,
                        out_h,
                        out_x,
                        out_y);

                fprintf(stderr,
                        "Playback pacing: %.3f fps\n",
                        fps);

                fprintf(stderr,
                        "Displaying %d frames...\n\n",
                        TARGET_VIDEO_FRAMES);

                framebuffer_clear(&fb);
            }

            /*
             * Pace video to the DVD frame rate.
             */
            int64_t now =
                av_gettime_relative();

            if (now < next_frame_us) {

                av_usleep(
                    (unsigned)(next_frame_us - now));

            } else if (now >
                       next_frame_us +
                       frame_period_us) {

                late_frames++;
            }

            stage = 7;

            int64_t scale_start =
                av_gettime_relative();

            sws_scale(sws,
                      (const uint8_t * const *)
                          frame->data,
                      frame->linesize,
                      0,
                      frame->height,
                      dst_data,
                      dst_linesize);

            int64_t scale_end =
                av_gettime_relative();

            /*
             * Deliberately do not copy to /dev/fb0.
             * This isolates YUV -> BGR0 conversion cost.
             */
            int64_t blit_end = scale_end;

            scale_cpu_us +=
                scale_end - scale_start;

            blit_cpu_us +=
                blit_end - scale_end;

            rendered++;

            next_frame_us +=
                frame_period_us;

            if ((rendered % 100) == 0) {

                fprintf(stderr,
                        "Displayed %d/%d frames\n",
                        rendered,
                        TARGET_VIDEO_FRAMES);
            }

            av_frame_unref(frame);

            if (rendered >=
                TARGET_VIDEO_FRAMES)
                break;
        }
    }

    /*
     * Put the Linux/MiSTer screen back exactly as
     * it was before the test.
     */
    framebuffer_close(&fb);

    double average_scale_ms =
        rendered > 0
            ? ((double)scale_cpu_us /
               (double)rendered) /
              1000.0
            : 0.0;

    double average_blit_ms =
        rendered > 0
            ? ((double)blit_cpu_us /
               (double)rendered) /
              1000.0
            : 0.0;

    fprintf(stderr,
            "\n=== VIDEO TEST COMPLETE ===\n");

    fprintf(stderr,
            "Frames displayed: %d\n",
            rendered);

    fprintf(stderr,
            "Warm-up frames: %d\n",
            warmup);

    fprintf(stderr,
            "Average swscale time: %.3f ms/frame\n",
            average_scale_ms);

    fprintf(stderr,
            "Average framebuffer blit: %.3f ms/frame\n",
            average_blit_ms);

    fprintf(stderr,
            "Combined rendering time: %.3f ms/frame\n",
            average_scale_ms + average_blit_ms);

    fprintf(stderr,
            "Late frames: %d\n",
            late_frames);

    fprintf(stderr,
            "DVD NAV packets: %lu\n",
            d.nav_packets);

    fprintf(stderr,
            "DVD MPEG sectors: %lu\n",
            d.mpeg_sectors);

    sws_freeContext(sws);

    if (dst_data[0])
        av_freep(&dst_data[0]);

    av_frame_free(&frame);
    av_packet_free(&pkt);

    avcodec_free_context(&vdec);

    avformat_close_input(&fmt);
    avio_context_free(&avio);

    dvdnav_close(d.nav);
    free(d.sector);

    if (rendered >=
        TARGET_VIDEO_FRAMES) {

        fprintf(stderr,
                "\nPASS: native DVD video displayed successfully.\n");

        return 0;
    }

    fprintf(stderr,
            "\nFAIL: did not display enough frames.\n");

    return 9;
}
