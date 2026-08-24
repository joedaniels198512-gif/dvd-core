/*
 * dvd_av_threaded_test.c — FROZEN threaded A/V proof (tag working-threaded-av).
 *
 * SS1, title 2 / chapter 1, ~30 s: continuous playback, normal speed, good
 * lip-sync. Do not retune queues, A/V clocks, decode, swscale, or mailbox
 * scheduling from this file.
 *
 * Proven hardware (this tag):
 *   30.872 s audio consumed, 0 MrAudio underruns, avg fill 139.2 ms
 *   771 frames displayed, avg video−audio offset −30.9 ms
 *   1 frame >80 ms late
 *
 * Known / non-blocking (not bugs; do not "fix"):
 *   −30.9 ms mean offset is mailbox-VBL presentation vs the audio master
 *     clock, with no frame dropping. Sync is visually good.
 *   Average fill 139.2 ms vs 150 ms target is healthy, not starvation.
 *   1 late frame in 771 is acceptable for this proof.
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
 * waits when a queue is genuinely full; this proof does not drop packets.
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
#include <libavutil/error.h>
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
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#define DVD_SECTOR      2048
#define AVIO_BUF_SIZE   (64 * 1024)
#define TARGET_SECONDS  30

#define OUT_RATE        48000
#define OUT_CHANNELS    2
#define OUT_BYTES       4
#define BYTES_PER_SEC   (OUT_RATE * OUT_BYTES)

#define MRAUDIO_DEV     "/dev/MrAudio"
#define WRITE_CHUNK     4096
#define TARGET_FILL     (BYTES_PER_SEC * 150 / 1000)  /* 28800 bytes      */
#define READY_FILL      (BYTES_PER_SEC * 100 / 1000)  /* 19200 bytes      */
#define PRIME_BYTES     256

#define AUDIO_Q_CAP     32
#define VIDEO_Q_CAP     384

#define FB_A_PHYS       0x30000000UL
#define FB_B_PHYS       0x30200000UL
#define MB_PHYS         0x30400000UL
#define FB_W            720
#define FB_H            576
#define FB_STRIDE       2880
#define FB_SIZE         ((size_t)FB_STRIDE * FB_H)
#define MB_MAP_SIZE     4096

#define STARTUP_DISPLAY_VBLS     2
#define MAILBOX_POLL_MARGIN_US   2000
#define VSYNC_SAMPLE_INTERVALS   30
#define DISC_JUMP_US             80000
#define EARLY_SLACK_US           2000
#define MAX_HOLD_US              500000
#define OFFSET_LOG_N             3

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

static volatile sig_atomic_t stage = 0;

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

/* ------------------------------------------------------------------ */
/* DVD -> AVIO  (demux thread only)                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    dvdnav_t *nav;
    uint8_t *sector;
    int sector_len, sector_pos, stopped;
    unsigned long nav_packets, mpeg_sectors;
} DVDIO;

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
        int32_t event = 0, len = 0;
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
    int title, chapter, list_titles;
} Cli;

static void usage(void)
{
    fprintf(stderr,
            "Usage: dvd_av_threaded_test [device] [--title N] [--chapter N]\n"
            "       dvd_av_threaded_test [device] --list-titles\n"
            "Defaults to /dev/sr0, title 2 / chapter 1.\n");
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
        fprintf(stderr, "  System RAM: 0x%09llx-0x%09llx\n", start, end);
        if (base <= end && (base + size - 1) >= start)
            overlap = 1;
    }
    fclose(fp);
    return checked ? overlap : -1;
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
        fprintf(stderr, "WARNING: could not verify via /proc/iomem.\n");
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
    fprintf(stderr,
            "\nMapping MISTER_FB double buffers + mailbox:\n"
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
    fprintf(stderr,
            "HDMI vsync: %d intervals in %.3f ms  ->  %.3f ms/period (%.3f Hz)\n",
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

static void pktq_push(PktQ *q, AVPacket *src)
{
    int64_t t0 = 0;
    pthread_mutex_lock(&q->mu);
    while (q->count >= q->cap && !q->quit) {
        if (!t0) {
            t0 = av_gettime_relative();
            q->full_n++;
        }
        pthread_cond_wait(&q->not_full, &q->mu);
    }
    if (t0)
        q->block_us += av_gettime_relative() - t0;
    if (q->quit) {
        pthread_mutex_unlock(&q->mu);
        return;
    }
    av_packet_ref(&q->pkts[q->tail], src);
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    q->pushed++;
    pktq_note_depth(q);
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

/* 1 = packet, 0 = eof/quit */
static int pktq_pop(PktQ *q, AVPacket *dst)
{
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->eof && !q->quit)
        pthread_cond_wait(&q->not_empty, &q->mu);
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
/* MrAudio (audio thread only, except clock snapshot)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int wr_fd, hw_pace;
    int64_t bytes_written, prime_bytes, short_writes;
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
                          int64_t last_apts_us, int fill, int hw_pace, int ready)
{
    pthread_mutex_lock(&c->mu);
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
    int64_t s = a->bytes_written - a->prime_bytes;
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
        fprintf(stderr, "MrAudio pacing: hardware rptr/len\n  first: %s",
                a->last_line);
    } else {
        a->hw_pace = 0;
        fprintf(stderr, "MrAudio rptr unavailable; wall-clock fallback.\n");
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
    fprintf(stderr, "Priming FPGA got_first with %d silence bytes.\n",
            PRIME_BYTES);
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

typedef struct {
    FBPair *fb;
    PktQ aq, vq;
    AudioClock clock;
    AVRational atb, vtb, fps, avg_fr, r_fr;
    int ai, vi;
    int fail;
    int audio_started, video_started;
    AVCodecParameters *acp, *vcp;
    MrAudio mr_stats;
    int64_t audio_cpu_us, video_cpu_us, demux_cpu_us;

    unsigned long audio_packets, audio_frames;
    int64_t src_samples, out_samples;
    int audio_disc, audio_missing_pts;
    int64_t last_audio_pts_us;

    int rendered, frames_a, frames_b, flips;
    int frames_late, late_40, late_80, video_disc, preroll_decoded;
    int64_t offset_sum, last_offset, offset_max_pos, offset_max_neg;
    int offset_n;
    int64_t first_offsets[OFFSET_LOG_N], last_offsets[OFFSET_LOG_N];
    int first_off_n, last_off_n;
    int64_t last_video_pts_us;
    int64_t first_genuine_pts, timeline_pts, assigned_pts;
    int genuine_pts_count, interpolated_count, fallback_frames;
    int64_t wait_us, vsync_us, convert_us;
    int displayed;
    int64_t present_vbl;
} Player;

static void player_abort(Player *p)
{
    p->fail = 1;
    pktq_quit(&p->aq);
    pktq_quit(&p->vq);
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
    fprintf(stderr, "avcodec_open2(%s) returned %d\n", codec->name, r);
    if (r < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

static int emit_pcm(MrAudio *mr, uint8_t *chunk, int *used,
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
            *used = 0;
        }
    }
    return 0;
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

    memset(&mr, 0, sizeof(mr));
    mr.wr_fd = -1;
    mr.rptr = mr.wptr = mr.fill = mr.comp = -1;

    stage = 7;
    if (!frame || !pkt) {
        player_abort(p);
        goto done;
    }
    if (mraudio_open(&mr) < 0 || mraudio_prime(&mr) < 0) {
        player_abort(p);
        goto done;
    }

    adec = open_decoder(p->acp, p->atb, AVMEDIA_TYPE_AUDIO);
    if (!adec) {
        player_abort(p);
        goto done;
    }

    while (pktq_pop(&p->aq, pkt)) {
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
                    fprintf(stderr,
                            "First decoded audio PTS: %.6f s (clock origin)\n",
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
                fprintf(stderr, "Audio convert: %s %d Hz %d ch -> 48 kHz S16 stereo\n",
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
                    if (emit_pcm(&mr, chunk, &chunk_used, out_data[0],
                                 got * OUT_BYTES) < 0) {
                        player_abort(p);
                        av_frame_unref(frame);
                        goto done;
                    }
                }
            }
            if (mr.hw_pace)
                mraudio_poll(&mr);
            {
                int64_t elapsed = mraudio_elapsed_us(&mr);
                int ready = (first_pts_us != AV_NOPTS_VALUE &&
                             mr.fill >= READY_FILL);
                clock_publish(&p->clock, first_pts_us, elapsed,
                              p->last_audio_pts_us, mr.fill, mr.hw_pace, ready);
            }
            av_frame_unref(frame);
        }
    }

    if (chunk_used > 0) {
        while (chunk_used & 3)
            chunk[chunk_used++] = 0;
        mraudio_wait_fill(&mr, chunk_used);
        mraudio_write_all(&mr, chunk, chunk_used);
        mr.playing = 1;
    }
    mraudio_drain(&mr);
    clock_publish(&p->clock, first_pts_us, mraudio_elapsed_us(&mr),
                  p->last_audio_pts_us, mr.fill, mr.hw_pace,
                  first_pts_us != AV_NOPTS_VALUE);

    fprintf(stderr,
            "Audio thread done: packets=%lu frames=%lu samples=%" PRId64
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

static void wait_until(Player *p, int64_t target)
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
        wait_vsync(p->fb->fb_fd);
        p->vsync_us += av_gettime_relative() - v0;
    }
    p->wait_us += av_gettime_relative() - t0;
}

static int64_t choose_present_vbl(Player *p, int64_t pts_target, int64_t now)
{
    int64_t min_k = p->present_vbl + 1;
    int64_t k = vbl_nearest(p->fb, pts_target);
    int64_t margin = p->fb->margin_us;
    if (k < min_k)
        k = min_k;
    while (vbl_time(p->fb, k) - margin < now)
        k++;
    return k;
}

static void record_offset(Player *p, int64_t off)
{
    if (p->first_off_n < OFFSET_LOG_N)
        p->first_offsets[p->first_off_n++] = off;
    if (p->last_off_n < OFFSET_LOG_N)
        p->last_offsets[p->last_off_n++] = off;
    else {
        memmove(p->last_offsets, p->last_offsets + 1,
                (OFFSET_LOG_N - 1) * sizeof(p->last_offsets[0]));
        p->last_offsets[OFFSET_LOG_N - 1] = off;
    }
    p->offset_sum += off;
    p->offset_n++;
    p->last_offset = off;
    if (p->offset_n == 1 || off > p->offset_max_pos)
        p->offset_max_pos = off;
    if (p->offset_n == 1 || off < p->offset_max_neg)
        p->offset_max_neg = off;
    if (off < 0)
        p->frames_late++;
    if (off < -40000)
        p->late_40++;
    if (off < -80000)
        p->late_80++;
}

static int present_video_frame(Player *p, AVFrame *frame, AVCodecContext *vdec,
                               struct SwsContext **sws, int timed)
{
    uint8_t *dst_data[4] = {0};
    int dst_linesize[4] = {0};

    if (frame->width != FB_W || frame->height != FB_H) {
        fprintf(stderr, "FAIL: %dx%d, expected %dx%d\n",
                frame->width, frame->height, FB_W, FB_H);
        player_abort(p);
        return -1;
    }
    if (!*sws) {
        AVRational fps0 = detect_fps(p, vdec);
        fprintf(stderr,
                "\n=== VIDEO PATH (threaded, audio-master) ===\n"
                "%dx%d %s  fps %d/%d (%.3f)  vsync %.3f ms\n",
                frame->width, frame->height,
                av_get_pix_fmt_name(frame->format)
                    ? av_get_pix_fmt_name(frame->format) : "?",
                fps0.num, fps0.den,
                (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0,
                (double)p->fb->vsync_period_us / 1000.0);
        *sws = sws_getContext(FB_W, FB_H, frame->format,
                              FB_W, FB_H, AV_PIX_FMT_BGR0,
                              SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!*sws) {
            fprintf(stderr, "sws_getContext failed\n");
            player_abort(p);
            return -1;
        }
    }

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

    if (p->present_vbl >= 0)
        wait_until(p, vbl_time(p->fb, p->present_vbl));

    if (timed && vpts_us != AV_NOPTS_VALUE) {
        int64_t hold0 = av_gettime_relative();
        for (;;) {
            int64_t aclk = clock_read(&p->clock, NULL, NULL);
            if (aclk == AV_NOPTS_VALUE)
                break;
            if (vpts_us <= aclk + EARLY_SLACK_US)
                break;
            if (av_gettime_relative() - hold0 > MAX_HOLD_US)
                break;
            {
                int64_t remain = vpts_us - aclk;
                if (remain > 5000)
                    remain = 5000;
                if (remain < 500)
                    remain = 500;
                av_usleep((unsigned)remain);
            }
        }
    }

    int next = p->displayed ^ 1;
    dst_data[0] = next ? p->fb->fb_b : p->fb->fb_a;
    dst_linesize[0] = FB_STRIDE;
    int64_t c0 = av_gettime_relative();
    sws_scale(*sws, (const uint8_t * const *)frame->data, frame->linesize,
              0, FB_H, dst_data, dst_linesize);
    p->convert_us += av_gettime_relative() - c0;
    __sync_synchronize();

    int64_t now = av_gettime_relative();
    int64_t aclk = clock_read(&p->clock, NULL, NULL);
    int64_t pts_target = now;
    if (timed && aclk != AV_NOPTS_VALUE && vpts_us != AV_NOPTS_VALUE) {
        int64_t off = vpts_us - aclk;
        pts_target = now + off;
        record_offset(p, off);
    }

    int64_t k = choose_present_vbl(p, pts_target, now);
    if (k > 0)
        wait_until(p, vbl_time(p->fb, k - 1));
    p->fb->mbox[0] = (uint32_t)next;
    if (next)
        p->frames_b++;
    else
        p->frames_a++;
    p->flips++;
    p->present_vbl = k;
    p->displayed = next;
    p->rendered++;
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

    p->present_vbl = -1;
    p->first_genuine_pts = AV_NOPTS_VALUE;
    p->timeline_pts = AV_NOPTS_VALUE;
    p->assigned_pts = AV_NOPTS_VALUE;
    p->last_video_pts_us = AV_NOPTS_VALUE;

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

    while (pktq_pop(&p->vq, pkt)) {
        const uint8_t *in = pkt->data;
        int in_size = pkt->size;
        int64_t in_pts = pkt->pts, in_dts = pkt->dts, in_pos = pkt->pos;

        if (!timed) {
            pthread_mutex_lock(&p->clock.mu);
            timed = p->clock.ready;
            pthread_mutex_unlock(&p->clock.mu);
            if (timed && !ready_logged) {
                fprintf(stderr, "Audio ring ready — starting timed video.\n");
                ready_logged = 1;
            }
        }

        while (in_size > 0 && !p->fail) {
            uint8_t *out_data = NULL;
            int out_size = 0;
            int used = av_parser_parse2(parser, vdec, &out_data, &out_size,
                                        in, in_size, in_pts, in_dts, in_pos);
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
            if (avcodec_send_packet(vdec, ppkt) < 0)
                continue;
            while (!p->fail && avcodec_receive_frame(vdec, frame) == 0) {
                if (!timed) {
                    p->preroll_decoded++;
                    av_frame_unref(frame);
                    continue;
                }
                if (present_video_frame(p, frame, vdec, &sws, 1) < 0) {
                    av_frame_unref(frame);
                    goto done;
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }

done:
    p->video_cpu_us = av_gettime_relative() - t0;
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

int main(int argc, char **argv)
{
    install_sigill_handler();
    setvbuf(stderr, NULL, _IONBF, 0);

    Cli cli;
    if (parse_cli(argc, argv, &cli) < 0)
        return 1;
    if (cli.list_titles) {
        dvdnav_t *nav = NULL;
        fprintf(stderr, "=== DVD TITLE LIST ===\nDevice: %s\n", cli.device);
        if (dvdnav_open(&nav, cli.device) != DVDNAV_STATUS_OK)
            return 1;
        int lr = list_dvd_titles(nav);
        dvdnav_close(nav);
        return lr < 0 ? 1 : 0;
    }

    int64_t program_start = av_gettime_relative();
    fprintf(stderr, "=== SS1 THREADED DVD A/V PROOF ===\n");
    fprintf(stderr, "FFmpeg CPU flags: 0x%x\n", av_get_cpu_flags());
    fprintf(stderr,
            "Queues: audio %d pkts (~1 s AC-3), video %d pkts (~1.1 s at ~340 pkt/s).\n"
            "Leave OSD Buffer on A.\n",
            AUDIO_Q_CAP, VIDEO_Q_CAP);

    av_log_set_level(AV_LOG_WARNING);

    FBPair fb;
    if (map_double_fb(&fb) < 0)
        return 11;

    DVDIO d;
    memset(&d, 0, sizeof(d));
    if (posix_memalign((void **)&d.sector, DVD_SECTOR, DVD_SECTOR) != 0)
        return 1;

    stage = 1;
    fprintf(stderr, "Opening DVD %s...\n", cli.device);
    if (dvdnav_open(&d.nav, cli.device) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "dvdnav_open failed\n");
        return 1;
    }
    dvdnav_set_readahead_flag(d.nav, 1);
    dvdnav_menu_language_select(d.nav, "en");
    dvdnav_audio_language_select(d.nav, "en");
    dvdnav_spu_language_select(d.nav, "en");
    if (!cli.title) {
        cli.title = 2;
        cli.chapter = 1;
    }
    fprintf(stderr, "Starting DVD title %d, chapter %d\n",
            cli.title, cli.chapter ? cli.chapter : 1);
    int32_t ntitles = 0;
    dvdnav_get_number_of_titles(d.nav, &ntitles);
    fprintf(stderr, "Titles on disc: %d\n", (int)ntitles);
    if (jump_to_title(d.nav, &cli, ntitles) < 0)
        return 1;

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
    fprintf(stderr, "MPEG-PS demuxer opened (demux thread owns it).\n");

    Player p;
    memset(&p, 0, sizeof(p));
    p.fb = &fb;
    p.ai = p.vi = -1;
    p.last_audio_pts_us = AV_NOPTS_VALUE;
    clock_init(&p.clock);
    if (pktq_init(&p.aq, AUDIO_Q_CAP) < 0 || pktq_init(&p.vq, VIDEO_Q_CAP) < 0)
        return 6;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return 6;

    pthread_t ath, vth;
    int last_diag_s = -1;
    const int64_t target_elapsed = (int64_t)TARGET_SECONDS * 1000000;
    int64_t demux_t0 = av_gettime_relative();
    int read_ret = 0;

    fprintf(stderr, "\nDemuxing (~%d s, backpressure on full queues)...\n",
            TARGET_SECONDS);

    while (!p.fail && (read_ret = av_read_frame(fmt, pkt)) >= 0) {
        int64_t elapsed = 0;
        pthread_mutex_lock(&p.clock.mu);
        elapsed = p.clock.elapsed_us;
        pthread_mutex_unlock(&p.clock.mu);
        if (elapsed >= target_elapsed)
            break;

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
                    fprintf(stderr, "Video stream #%d %s tb %d/%d\n",
                            p.vi, avcodec_get_name(p.vcp->codec_id),
                            p.vtb.num, p.vtb.den);
                }
                if (p.ai < 0 && cp->codec_type == AVMEDIA_TYPE_AUDIO) {
                    p.ai = (int)i;
                    p.atb = fmt->streams[i]->time_base;
                    if (copy_codecpar(&p.acp, cp) < 0) {
                        player_abort(&p);
                        break;
                    }
                    fprintf(stderr, "Audio stream #%d %s %d Hz %d ch tb %d/%d\n",
                            p.ai, avcodec_get_name(p.acp->codec_id),
                            p.acp->sample_rate, p.acp->ch_layout.nb_channels,
                            p.atb.num, p.atb.den);
                }
            }
        }

        if (p.ai >= 0 && p.acp && !p.audio_started) {
            if (pthread_create(&ath, NULL, audio_thread, &p) != 0) {
                fprintf(stderr, "pthread_create(audio) failed\n");
                player_abort(&p);
                break;
            }
            p.audio_started = 1;
            fprintf(stderr, "Audio worker started.\n");
        }
        if (p.vi >= 0 && p.vcp && !p.video_started) {
            if (pthread_create(&vth, NULL, video_thread, &p) != 0) {
                fprintf(stderr, "pthread_create(video) failed\n");
                player_abort(&p);
                break;
            }
            p.video_started = 1;
            fprintf(stderr, "Video worker started.\n");
        }

        if (p.audio_started && pkt->stream_index == p.ai)
            pktq_push(&p.aq, pkt);
        else if (p.video_started && pkt->stream_index == p.vi)
            pktq_push(&p.vq, pkt);

        av_packet_unref(pkt);

        {
            int64_t elapsed = 0, apts = 0, aclk = 0;
            int fill = 0;
            pthread_mutex_lock(&p.clock.mu);
            elapsed = p.clock.elapsed_us;
            apts = p.clock.last_apts_us;
            aclk = p.clock.clock_us;
            fill = p.clock.fill;
            pthread_mutex_unlock(&p.clock.mu);
            int sec = (int)(elapsed / 1000000);
            if (sec != last_diag_s && (sec % 2) == 0 && sec > 0) {
                last_diag_s = sec;
                fprintf(stderr,
                        "  t=%ds  aclk=%.3fs apts=%.3fs vpts=%.3fs v-a=%+.1fms"
                        "  fill=%d (%.0fms)  qA=%d qV=%d  frames=%d late=%d\n",
                        sec,
                        aclk == AV_NOPTS_VALUE ? 0.0 : aclk / 1e6,
                        apts == AV_NOPTS_VALUE ? 0.0 : apts / 1e6,
                        p.last_video_pts_us == AV_NOPTS_VALUE
                            ? 0.0 : p.last_video_pts_us / 1e6,
                        p.last_offset / 1000.0,
                        fill, fill * 1000.0 / (double)BYTES_PER_SEC,
                        pktq_count(&p.aq), pktq_count(&p.vq),
                        p.rendered, p.frames_late);
            }
        }
    }

    p.demux_cpu_us = av_gettime_relative() - demux_t0;
    pktq_eof(&p.aq);
    pktq_eof(&p.vq);
    if (p.audio_started)
        pthread_join(ath, NULL);
    if (p.video_started)
        pthread_join(vth, NULL);

    int64_t wall_us = av_gettime_relative() - program_start;
    double wall = wall_us / 1e6;
    const MrAudio *mr = &p.mr_stats;
    double hw_dur = (double)mraudio_consumed(mr) / (double)BYTES_PER_SEC;
    double fill_avg = mr->fill_n ? (double)mr->fill_sum / mr->fill_n : 0.0;
    double aq_avg = p.aq.depth_n ? (double)p.aq.depth_sum / p.aq.depth_n : 0.0;
    double vq_avg = p.vq.depth_n ? (double)p.vq.depth_sum / p.vq.depth_n : 0.0;
    double off_avg = p.offset_n ? (double)p.offset_sum / p.offset_n : 0.0;

    fprintf(stderr,
            "\n=== THREADED A/V REPORT ===\n"
            "Wall playback duration:     %8.3f s\n"
            "Hardware audio consumed:    %8.3f s\n"
            "Audio samples written:      %" PRId64 "  (%.3f s)\n"
            "Audio queue depth min/avg/max: %d / %.1f / %d\n"
            "Video queue depth min/avg/max: %d / %.1f / %d\n"
            "Audio queue full:           %lu times, demux wait %.3f s\n"
            "Video queue full:           %lu times, demux wait %.3f s\n"
            "Demux backpressure wait:    %.3f s  (A %.3f + V %.3f)\n"
            "MrAudio fill min/avg/max:   %" PRId64 " / %.0f / %" PRId64
            " bytes (%.1f / %.1f / %.1f ms)\n"
            "MrAudio underruns:          %d  (%.3f s)\n"
            "Audio packets/frames:       %lu / %lu\n"
            "Video packets/frames:       %lu / %d  (preroll decoded %d)\n"
            "Average video-audio offset: %+.2f ms\n"
            "Max video ahead/behind:     %+.2f / %+.2f ms\n"
            "Frames >40 ms / >80 ms late:%d / %d\n"
            "Audio/video PTS breaks:     %d / %d\n"
            "Thread CPU (wall in thread): demux %.3fs  audio %.3fs  video %.3fs\n"
            "Pacing:                     %s\n",
            wall, hw_dur, p.out_samples,
            (double)p.out_samples / (double)OUT_RATE,
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
            p.audio_packets, p.audio_frames,
            p.vq.pushed, p.rendered, p.preroll_decoded,
            off_avg / 1000.0,
            (double)p.offset_max_pos / 1000.0,
            (double)p.offset_max_neg / 1000.0,
            p.late_40, p.late_80,
            p.audio_disc, p.video_disc,
            p.demux_cpu_us / 1e6, p.audio_cpu_us / 1e6, p.video_cpu_us / 1e6,
            mr->hw_pace ? "hardware FPGA rptr/len" : "DEGRADED wall-clock");

    fprintf(stderr, "A/V offset near start (ms):");
    for (int i = 0; i < p.first_off_n; i++)
        fprintf(stderr, "  %+.1f", p.first_offsets[i] / 1000.0);
    fprintf(stderr, "%s\n", p.first_off_n ? "" : "  (none)");
    fprintf(stderr, "A/V offset near end (ms):  ");
    for (int i = 0; i < p.last_off_n; i++)
        fprintf(stderr, "  %+.1f", p.last_offsets[i] / 1000.0);
    fprintf(stderr, "%s\n", p.last_off_n ? "" : "  (none)");

    if (read_ret < 0) {
        char e[128];
        fferr(read_ret, e, sizeof(e));
        fprintf(stderr, "Demux ended: %s\n", e);
    }

    pktq_free(&p.aq);
    pktq_free(&p.vq);
    avcodec_parameters_free(&p.acp);
    avcodec_parameters_free(&p.vcp);
    pthread_mutex_destroy(&p.clock.mu);
    pthread_cond_destroy(&p.clock.ready_cv);
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    avio_context_free(&avio);
    dvdnav_close(d.nav);
    free(d.sector);
    unmap_double_fb(&fb);

    if (p.fail)
        return 10;
    if (p.rendered <= 0 || p.out_samples <= 0)
        return 9;
    fprintf(stderr,
            "\nPASS: threaded A/V proof finished (%.1f s wall / %.1f s audio).\n",
            wall, hw_dur);
    return 0;
}
