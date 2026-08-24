/*
 * dvd_audio_test.c — AUDIO PROOF ONLY.
 *
 * Decode ~20 s of DVD audio from title 2 / chapter 1 and play it through
 * /dev/MrAudio. No video, no A/V clock, no FPGA/mailbox changes.
 *
 * Pipeline:
 *   /dev/sr0 -> libdvdnav -> custom AVIO -> MPEG-PS demux
 *   -> first discovered audio stream (not a hardcoded index)
 *   -> FFmpeg decode -> libswresample -> 48 kHz S16 stereo interleaved
 *   -> paced writes to /dev/MrAudio
 *
 * /dev/MrAudio semantics (from Linux-Kernel_MiSTer sound/drivers/
 * MiSTer-audio-spi.c and fpga/sys/alsa.sv — not guessed):
 *
 *   write() copies PCM into a 512 KB CMA ring and SPIs the new write
 *   pointer. It does NOT block on fullness and does NOT drop; overwriting
 *   unread data is possible. Length is forced 4-byte aligned.
 *
 *   open()+read() returns one snapshot line:
 *     "rptr: %d, wptr: %d, len: %d, comp: %d\n"
 *   rptr is the FPGA read pointer (SPI). len is current fill. The snapshot
 *   is taken at open(), so a later read on the same fd is stale / EOF;
 *   this test re-opens O_RDONLY for each poll and keeps a separate
 *   O_WRONLY fd for writes.
 *
 *   FPGA alsa.sv consumes the ring at 48 kHz. On the first non-empty
 *   buffer it snaps rptr to wptr (discards that first write), then plays.
 *
 *   Main_MiSTer does not write PCM here (volume/filter SPI only). Linux
 *   software paces by polling rptr/len. That is the hardware-paced path.
 *
 * Requires: a sys_top core with alsa.sv (DVD.rbf is fine) and a DVD in
 * /dev/sr0. libswresample is already in the current VFPv3 FFmpeg prefix.
 */

#define _GNU_SOURCE

#include <dvdnav/dvdnav.h>
#include <dvdnav/dvdnav_events.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/cpu.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#define DVD_SECTOR      2048
#define AVIO_BUF_SIZE   (64 * 1024)

#define TARGET_SECONDS  20
#define OUT_RATE        48000
#define OUT_CHANNELS    2
#define OUT_BYTES       4                 /* S16LE stereo interleaved     */
#define BYTES_PER_SEC   (OUT_RATE * OUT_BYTES)

#define MRAUDIO_DEV     "/dev/MrAudio"
#define MRAUDIO_RING    (512 * 1024)      /* kernel BUFFER_LEN            */
#define WRITE_CHUNK     4096              /* 1024 frames = 21.333 ms      */
#define TARGET_FILL     (BYTES_PER_SEC * 150 / 1000)  /* ~150 ms          */
#define PRIME_BYTES     256               /* discarded by FPGA got_first  */

#define DISC_JUMP_US    80000             /* PTS jump treated as a break  */

static volatile sig_atomic_t stage = 0;

static const char *stage_names[] = {
    "startup",
    "dvdnav",
    "mpeg demux",
    "packet read",
    "audio decoder",
    "frame decode",
    "resample",
    "mraudio"
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

/* ------------------------------------------------------------------ */
/* DVD -> AVIO (same plumbing as the video tests)                      */
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

typedef struct {
    const char *device;
    int title;
    int chapter;
    int list_titles;
} Cli;

static void usage(void)
{
    fprintf(stderr,
            "Usage: dvd_audio_test [device] [--title N] [--chapter N]\n"
            "       dvd_audio_test [device] --list-titles\n"
            "device defaults to /dev/sr0.\n"
            "The benchmark always starts title 2 / chapter 1 unless overridden.\n");
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
        fprintf(stderr, "Chapters in title %d: %d\n", cli->title, (int)parts);

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
/* /dev/MrAudio                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int wr_fd;
    int hw_pace;              /* 1 = paced from FPGA rptr/len         */
    int chunk;
    int64_t bytes_written;
    int64_t short_writes;
    int write_errors;
    int polls;
    int rptr;
    int wptr;
    int fill;
    int comp;
    char last_line[128];
    int64_t wall_origin;      /* used only if hw_pace == 0            */
} MrAudio;

static int mraudio_poll(MrAudio *a)
{
    char buf[128];
    int fd;
    ssize_t n;
    int rptr = -1, wptr = -1, fill = -1, comp = -1;

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
    return 0;
}

static int mraudio_open(MrAudio *a)
{
    memset(a, 0, sizeof(*a));
    a->chunk = WRITE_CHUNK;
    a->wr_fd = -1;
    a->rptr = a->wptr = a->fill = a->comp = -1;

    stage = 7;

    a->wr_fd = open(MRAUDIO_DEV, O_WRONLY | O_CLOEXEC);
    if (a->wr_fd < 0) {
        perror("open " MRAUDIO_DEV);
        return -1;
    }

    if (mraudio_poll(a) == 0) {
        a->hw_pace = 1;
        fprintf(stderr,
                "MrAudio pacing:           hardware rptr/len (FPGA 48 kHz)\n"
                "  ring size:              %d bytes  (~%.2f s @ 48 kHz S16 stereo)\n"
                "  write() blocks:         no (copies into CMA ring + SPI wptr)\n"
                "  fill snapshot:          open()+read() on %s\n"
                "  first status:           %s",
                MRAUDIO_RING,
                (double)MRAUDIO_RING / (double)BYTES_PER_SEC,
                MRAUDIO_DEV,
                a->last_line);
    } else {
        a->hw_pace = 0;
        fprintf(stderr,
                "MrAudio pacing:           WALL-CLOCK FALLBACK\n"
                "  Could not parse FPGA rptr/len from open()+read().\n"
                "  AUDIO PROOF ONLY — not the final A/V clock.\n");
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
                fprintf(stderr,
                        "MrAudio rptr poll failed; switching to wall-clock.\n"
                        "AUDIO PROOF ONLY — not the final A/V clock.\n");
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
        int64_t due = a->wall_origin +
                      (a->bytes_written * 1000000LL) / BYTES_PER_SEC;
        int64_t now = av_gettime_relative();

        if (due > now)
            av_usleep((unsigned)(due - now));
    }
}

static int mraudio_prime(MrAudio *a)
{
    uint8_t silence[PRIME_BYTES];

    memset(silence, 0, sizeof(silence));

    fprintf(stderr,
            "Priming FPGA got_first snap with %d bytes of silence "
            "(that first write is discarded by alsa.sv).\n",
            PRIME_BYTES);

    if (mraudio_write_all(a, silence, PRIME_BYTES) < 0)
        return -1;

    av_usleep(5000);
    if (a->hw_pace && mraudio_poll(a) == 0)
        fprintf(stderr, "  status after prime:   %s", a->last_line);

    a->bytes_written = 0;
    a->wall_origin = av_gettime_relative();
    return 0;
}

static void mraudio_drain(MrAudio *a)
{
    int64_t t0 = av_gettime_relative();

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

    if (a->hw_pace && a->last_line[0])
        fprintf(stderr, "  status after drain:   %s", a->last_line);
}

static void mraudio_close(MrAudio *a)
{
    if (a->wr_fd >= 0) {
        close(a->wr_fd);
        a->wr_fd = -1;
    }
}

static int emit_pcm(MrAudio *mr, uint8_t *chunk_buf, int *chunk_used,
                    const uint8_t *src, int remain,
                    int64_t *play_start, int *play_started)
{
    if (!*play_started) {
        *play_start = av_gettime_relative();
        *play_started = 1;
    }

    while (remain > 0) {
        int space = WRITE_CHUNK - *chunk_used;
        int take = remain < space ? remain : space;

        memcpy(chunk_buf + *chunk_used, src, (size_t)take);
        *chunk_used += take;
        src += take;
        remain -= take;

        if (*chunk_used == WRITE_CHUNK) {
            mraudio_wait_fill(mr, WRITE_CHUNK);
            if (mraudio_write_all(mr, chunk_buf, WRITE_CHUNK) < 0)
                return -1;
            *chunk_used = 0;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Decode + convert                                                    */
/* ------------------------------------------------------------------ */

static AVCodecContext *open_audio_decoder(AVStream *st)
{
    AVCodecParameters *cp = st->codecpar;
    const AVCodec *codec;
    AVCodecContext *ctx;
    int r;

    stage = 4;

    codec = avcodec_find_decoder(cp->codec_id);
    if (!codec) {
        fprintf(stderr, "No decoder for codec id %d (%s)\n",
                (int)cp->codec_id, avcodec_get_name(cp->codec_id));
        return NULL;
    }

    ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return NULL;

    if (avcodec_parameters_to_context(ctx, cp) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }

    ctx->pkt_timebase = st->time_base;

    r = avcodec_open2(ctx, codec, NULL);
    fprintf(stderr, "avcodec_open2(%s) returned %d\n", codec->name, r);

    if (r < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }

    return ctx;
}

static int64_t pts_to_us(AVRational tb, int64_t pts)
{
    if (pts == AV_NOPTS_VALUE || tb.num <= 0 || tb.den <= 0)
        return AV_NOPTS_VALUE;

    return av_rescale_q(pts, tb, (AVRational){1, 1000000});
}

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

    fprintf(stderr, "=== SS1 DVD AUDIO PROOF (/dev/MrAudio) ===\n");
    fprintf(stderr, "FFmpeg CPU flags: 0x%x\n", av_get_cpu_flags());
    fprintf(stderr, "No video. AUDIO PROOF ONLY — not the final A/V clock.\n");

    av_log_set_level(AV_LOG_WARNING);

    DVDIO d;
    memset(&d, 0, sizeof(d));

    if (posix_memalign((void **)&d.sector, DVD_SECTOR, DVD_SECTOR) != 0 ||
        !d.sector) {
        fprintf(stderr, "Could not allocate aligned DVD sector\n");
        return 1;
    }

    stage = 1;
    fprintf(stderr, "Opening DVD %s...\n", cli.device);

    if (dvdnav_open(&d.nav, cli.device) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "dvdnav_open failed\n");
        free(d.sector);
        return 1;
    }

    dvdnav_set_readahead_flag(d.nav, 1);
    dvdnav_menu_language_select(d.nav, "en");
    dvdnav_audio_language_select(d.nav, "en");
    dvdnav_spu_language_select(d.nav, "en");

    fprintf(stderr, "libdvdnav opened successfully\n");

    if (!cli.title) {
        cli.title = 2;
        cli.chapter = 1;
    }

    fprintf(stderr, "Starting DVD title %d, chapter %d\n",
            cli.title, cli.chapter ? cli.chapter : 1);

    int32_t ntitles = 0;

    if (dvdnav_get_number_of_titles(d.nav, &ntitles) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "dvdnav_get_number_of_titles failed: %s\n",
                dvdnav_err_to_string(d.nav));
        ntitles = 0;
    }

    fprintf(stderr, "Titles on disc: %d\n", (int)ntitles);

    if (jump_to_title(d.nav, &cli, ntitles) < 0)
        return 1;

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

    MrAudio mr;
    if (mraudio_open(&mr) < 0)
        return 8;
    if (mraudio_prime(&mr) < 0)
        return 8;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    if (!pkt || !frame)
        return 6;

    AVCodecContext *adec = NULL;
    SwrContext *swr = NULL;
    AVStream *ast = NULL;
    int ai = -1;

    uint8_t **out_data = NULL;
    int out_linesize = 0;
    int out_cap = 0;

    uint8_t chunk_buf[WRITE_CHUNK];
    int chunk_used = 0;

    unsigned long audio_packets = 0;
    int decoded_frames = 0;
    int64_t src_samples = 0;
    int64_t out_samples = 0;
    int64_t first_pkt_pts = AV_NOPTS_VALUE;
    int64_t first_frm_pts = AV_NOPTS_VALUE;
    int64_t last_frm_pts = AV_NOPTS_VALUE;
    int64_t last_frm_pts_us = AV_NOPTS_VALUE;
    int missing_pts = 0;
    int discontinuities = 0;
    int inspect_printed = 0;
    int last_status_s = -1;
    const int64_t target_out = (int64_t)TARGET_SECONDS * OUT_RATE;
    int64_t play_start = 0;
    int play_started = 0;

    fprintf(stderr, "\nReading DVD stream (audio only, ~%d s)...\n",
            TARGET_SECONDS);

    stage = 3;

    int read_ret = 0;

    while (out_samples < target_out &&
           (read_ret = av_read_frame(fmt, pkt)) >= 0) {

        if (ai < 0) {
            for (unsigned i = 0; i < fmt->nb_streams; ++i) {
                AVCodecParameters *cp = fmt->streams[i]->codecpar;

                if (cp->codec_type != AVMEDIA_TYPE_AUDIO)
                    continue;

                ai = (int)i;
                ast = fmt->streams[i];

                {
                    char layout[128] = "unknown";

                    if (cp->ch_layout.nb_channels > 0)
                        av_channel_layout_describe(&cp->ch_layout,
                                                   layout, sizeof(layout));

                    fprintf(stderr,
                            "\n=== AUDIO STREAM (discovered, not hardcoded) ===\n"
                            "Audio stream index:     %d\n"
                            "Codec ID/name:          %d / %s\n"
                            "Sample rate:            %d Hz\n"
                            "Channels:               %d\n"
                            "Channel layout:         %s\n"
                            "Codec/stream timebase:  %d/%d\n"
                            "First packet PTS:       (pending)\n",
                            ai,
                            (int)cp->codec_id,
                            avcodec_get_name(cp->codec_id),
                            cp->sample_rate,
                            cp->ch_layout.nb_channels,
                            layout,
                            ast->time_base.num, ast->time_base.den);
                }

                adec = open_audio_decoder(ast);
                if (!adec)
                    return 7;

                inspect_printed = 1;
                break;
            }
        }

        if (!adec || pkt->stream_index != ai) {
            av_packet_unref(pkt);
            continue;
        }

        audio_packets++;

        if (first_pkt_pts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE) {
            first_pkt_pts = pkt->pts;
            fprintf(stderr, "First audio packet PTS:  %" PRId64 "  (%.6f s)\n",
                    first_pkt_pts,
                    first_pkt_pts * av_q2d(ast->time_base));
        }

        stage = 5;

        if (avcodec_send_packet(adec, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }

        av_packet_unref(pkt);

        while (out_samples < target_out &&
               avcodec_receive_frame(adec, frame) == 0) {

            decoded_frames++;
            src_samples += frame->nb_samples;

            if (frame->pts == AV_NOPTS_VALUE) {
                missing_pts++;
            } else {
                int64_t pus = pts_to_us(ast->time_base, frame->pts);

                if (first_frm_pts == AV_NOPTS_VALUE)
                    first_frm_pts = frame->pts;

                if (last_frm_pts_us != AV_NOPTS_VALUE && pus != AV_NOPTS_VALUE) {
                    int64_t gap = pus - last_frm_pts_us;
                    if (gap < 0)
                        gap = -gap;
                    if (gap > DISC_JUMP_US)
                        discontinuities++;
                }

                last_frm_pts = frame->pts;
                last_frm_pts_us = pus;
            }

            if (!swr) {
                AVChannelLayout out_layout;

                av_channel_layout_default(&out_layout, OUT_CHANNELS);

                stage = 6;

                if (swr_alloc_set_opts2(&swr,
                                        &out_layout, AV_SAMPLE_FMT_S16, OUT_RATE,
                                        &frame->ch_layout,
                                        (enum AVSampleFormat)frame->format,
                                        frame->sample_rate,
                                        0, NULL) < 0 ||
                    !swr || swr_init(swr) < 0) {
                    fprintf(stderr, "swr_alloc_set_opts2 / swr_init failed\n");
                    av_channel_layout_uninit(&out_layout);
                    return 7;
                }

                av_channel_layout_uninit(&out_layout);

                fprintf(stderr,
                        "\n=== CONVERSION ===\n"
                        "libswresample:          already present in current FFmpeg build\n"
                        "Source:                 %s  %d Hz  %d ch\n"
                        "Output PCM:             48000 Hz  S16LE  stereo interleaved "
                        "(%d bytes/frame)\n",
                        av_get_sample_fmt_name(frame->format)
                            ? av_get_sample_fmt_name(frame->format) : "?",
                        frame->sample_rate,
                        frame->ch_layout.nb_channels,
                        OUT_BYTES);
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
                            &out_data, &out_linesize,
                            OUT_CHANNELS, max_out, AV_SAMPLE_FMT_S16, 0) < 0)
                        return 6;
                    out_cap = max_out;
                }

                int got = swr_convert(swr, out_data, max_out,
                                      (const uint8_t **)frame->extended_data,
                                      frame->nb_samples);

                if (got <= 0) {
                    av_frame_unref(frame);
                    continue;
                }

                out_samples += got;

                if (emit_pcm(&mr, chunk_buf, &chunk_used, out_data[0],
                             got * OUT_BYTES, &play_start, &play_started) < 0)
                    return 8;
            }

            {
                int sec = (int)(out_samples / OUT_RATE);

                if (sec != last_status_s && (sec % 2) == 0 && sec > 0) {
                    last_status_s = sec;
                    if (mr.hw_pace)
                        mraudio_poll(&mr);
                    fprintf(stderr,
                            "  %3d s  out_samples=%" PRId64
                            "  written=%" PRId64 "  fill=%d  rptr=%d wptr=%d\n",
                            sec, out_samples, mr.bytes_written,
                            mr.fill, mr.rptr, mr.wptr);
                }
            }

            av_frame_unref(frame);
        }
    }

    if (adec && swr && out_data) {
        avcodec_send_packet(adec, NULL);
        while (out_samples < target_out &&
               avcodec_receive_frame(adec, frame) == 0) {
            int max_out = (int)av_rescale_rnd(
                swr_get_delay(swr, frame->sample_rate) + frame->nb_samples,
                OUT_RATE, frame->sample_rate, AV_ROUND_UP);
            int got;

            decoded_frames++;
            src_samples += frame->nb_samples;

            if (max_out > out_cap)
                max_out = out_cap;

            got = swr_convert(swr, out_data, max_out,
                              (const uint8_t **)frame->extended_data,
                              frame->nb_samples);
            if (got > 0) {
                out_samples += got;
                if (emit_pcm(&mr, chunk_buf, &chunk_used, out_data[0],
                             got * OUT_BYTES, &play_start, &play_started) < 0)
                    return 8;
            }
            av_frame_unref(frame);
        }
    }

    if (swr && out_data) {
        int got;

        while ((got = swr_convert(swr, out_data, out_cap, NULL, 0)) > 0 &&
               out_samples < target_out) {
            out_samples += got;
            if (emit_pcm(&mr, chunk_buf, &chunk_used, out_data[0],
                         got * OUT_BYTES, &play_start, &play_started) < 0)
                return 8;
        }
    }

    if (chunk_used > 0) {
        while (chunk_used & 3)
            chunk_buf[chunk_used++] = 0;
        mraudio_wait_fill(&mr, chunk_used);
        if (mraudio_write_all(&mr, chunk_buf, chunk_used) < 0)
            return 8;
        chunk_used = 0;
    }

    fprintf(stderr, "Draining FPGA ring so the tail is audible...\n");
    mraudio_drain(&mr);

    {
        int64_t play_end = av_gettime_relative();
        double out_dur = (double)out_samples / (double)OUT_RATE;
        double wall = play_started
                      ? (double)(play_end - play_start) / 1e6
                      : (double)(play_end - program_start) / 1e6;
        double first_s = 0.0, last_s = 0.0;

        if (ast && first_frm_pts != AV_NOPTS_VALUE)
            first_s = first_frm_pts * av_q2d(ast->time_base);
        if (ast && last_frm_pts != AV_NOPTS_VALUE)
            last_s = last_frm_pts * av_q2d(ast->time_base);

        if (!inspect_printed)
            fprintf(stderr, "\nNo audio stream discovered.\n");

        fprintf(stderr,
                "\n=== AUDIO STATISTICS ===\n"
                "Audio packets:            %lu\n"
                "Decoded audio frames:     %d\n"
                "Decoded source samples:   %" PRId64 "\n"
                "Output samples (48 kHz):  %" PRId64 "\n"
                "Output duration:          %.3f s\n"
                "First audio PTS:          %" PRId64 "  (%.6f s)\n"
                "Last audio PTS:           %" PRId64 "  (%.6f s)\n"
                "Missing PTS frames:       %d\n"
                "PTS discontinuities:      %d  (jump > %d ms)\n"
                "DVD NAV packets:          %lu\n"
                "DVD MPEG sectors:         %lu\n",
                audio_packets, decoded_frames, src_samples, out_samples, out_dur,
                first_frm_pts, first_s, last_frm_pts, last_s,
                missing_pts, discontinuities, DISC_JUMP_US / 1000,
                d.nav_packets, d.mpeg_sectors);

        fprintf(stderr,
                "\n=== /dev/MrAudio ===\n"
                "Output PCM:               48000 Hz S16LE stereo interleaved\n"
                "Write chunk size:         %d bytes\n"
                "Total bytes written:      %" PRId64 "\n"
                "Short writes:             %" PRId64 "\n"
                "Write errors:             %d\n"
                "rptr polls:               %d\n"
                "Last rptr/wptr/fill/comp: %d / %d / %d / %d\n"
                "Pacing:                   %s\n"
                "Wall playback time:       %.3f s\n",
                WRITE_CHUNK, mr.bytes_written, mr.short_writes, mr.write_errors,
                mr.polls, mr.rptr, mr.wptr, mr.fill, mr.comp,
                mr.hw_pace
                    ? "hardware FPGA rptr/len (alsa.sv 48 kHz)"
                    : "temporary wall-clock (AUDIO PROOF ONLY)",
                wall);

        if (read_ret < 0 && out_samples < target_out) {
            char e[128];
            fferr(read_ret, e, sizeof(e));
            fprintf(stderr, "Demux ended early: %s\n", e);
        }
    }

    mraudio_close(&mr);

    if (out_data)
        av_freep(&out_data[0]);
    av_freep(&out_data);
    swr_free(&swr);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&adec);
    avformat_close_input(&fmt);
    avio_context_free(&avio);
    dvdnav_close(d.nav);
    free(d.sector);

    if (out_samples <= 0)
        return 9;

    fprintf(stderr,
            "\nPASS: wrote %.3f s of 48 kHz S16 stereo to /dev/MrAudio.\n"
            "AUDIO PROOF ONLY — not the final A/V clock.\n",
            (double)out_samples / (double)OUT_RATE);
    return 0;
}
