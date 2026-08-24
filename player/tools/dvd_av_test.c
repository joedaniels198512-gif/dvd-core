/*
 * dvd_av_test.c — FAILED single-thread A/V proof. Keep as the baseline
 * that demonstrated MrAudio starvation (fill 0 / 3.6 / 146 ms, 30 s media
 * in 146 s wall). Do not patch this scheduler. The threaded follow-up is
 * dvd_av_threaded_test.c.
 *
 * Title 2 / chapter 1, ~30 s. Audio is the master clock.
 * Does not replace dvd_video_double_test.c or dvd_audio_test.c.
 *
 * Single MPEG-PS demux loop feeds both:
 *   video  -> explicit MPEG-2 parser -> decode -> 720x576 BGR0
 *             -> mailbox A/B vblank flip
 *   audio  -> AC-3 decode -> libswresample 48 kHz S16 stereo
 *             -> /dev/MrAudio paced at ~150 ms fill via rptr/len
 *
 * Hardware audio clock:
 *   submitted = bytes written to MrAudio minus the 256-byte FPGA
 *               got_first silence prime
 *   consumed  = submitted - ring fill
 *   elapsed   = consumed / (48000 * 4)
 *   clock     = first decoded audio PTS + elapsed
 * Video early/late is decided against that clock, then the existing
 * 60 Hz VBL mailbox scheduler picks the publication edge.
 *
 * Single-threaded: the proven ~150 ms ring covers decode + dest-free
 * + short early-holds without a second thread or packet queue. Long
 * DVD-read stalls can still underrun; this proof does not restructure
 * for that.
 *
 * No menus, controllers, subtitles, frame dropping, FPGA changes.
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
#define FALLBACK_FPS    25

#define OUT_RATE        48000
#define OUT_CHANNELS    2
#define OUT_BYTES       4
#define BYTES_PER_SEC   (OUT_RATE * OUT_BYTES)

#define MRAUDIO_DEV     "/dev/MrAudio"
#define MRAUDIO_RING    (512 * 1024)
#define WRITE_CHUNK     4096
#define TARGET_FILL     (BYTES_PER_SEC * 150 / 1000)
#define PRIME_BYTES     256

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
    "startup",
    "dvdnav",
    "mpeg demux",
    "packet read",
    "decoder",
    "frame decode",
    "ddr framebuffer",
    "mraudio/swscale"
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

/* ------------------------------------------------------------------ */
/* DVD -> AVIO                                                         */
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
            "Usage: dvd_av_test [device] [--title N] [--chapter N]\n"
            "       dvd_av_test [device] --list-titles\n"
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
/* Framebuffer / mailbox                                               */
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
/* MrAudio + hardware clock                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int wr_fd, hw_pace, chunk;
    int64_t bytes_written;        /* includes prime                     */
    int64_t prime_bytes;          /* FPGA got_first snap, not media     */
    int64_t short_writes;
    int write_errors, polls;
    int rptr, wptr, fill, comp;
    char last_line[128];
    int64_t wall_origin;
    int64_t fill_min, fill_max, fill_sum;
    int fill_n;
} MrAudio;

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
                "  first status:           %s",
                a->last_line);
    } else {
        a->hw_pace = 0;
        fprintf(stderr,
                "MrAudio rptr unavailable; wall-clock fill pacing only.\n"
                "  AUDIO CLOCK DEGRADED — not the intended A/V master.\n");
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
                fprintf(stderr, "MrAudio rptr poll failed; wall-clock fallback.\n");
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
    fprintf(stderr,
            "Priming FPGA got_first with %d silence bytes "
            "(excluded from the media clock).\n", PRIME_BYTES);
    if (mraudio_write_all(a, silence, PRIME_BYTES) < 0)
        return -1;
    a->prime_bytes = PRIME_BYTES;
    av_usleep(5000);
    if (a->hw_pace && mraudio_poll(a) == 0)
        fprintf(stderr, "  status after prime:   %s", a->last_line);
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
}

static void mraudio_close(MrAudio *a)
{
    if (a->wr_fd >= 0) {
        close(a->wr_fd);
        a->wr_fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Combined player                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    FBPair *fb;
    MrAudio *mr;

    struct SwsContext *sws;
    uint8_t *dst_data[4];
    int dst_linesize[4];
    int displayed;
    int64_t present_vbl;
    int rendered, frames_a, frames_b, flips, fail;
    int64_t wait_us, vsync_us, convert_us;

    AVRational vtb, fps, avg_frame_rate, r_frame_rate;
    int64_t first_genuine_pts, timeline_pts, assigned_pts;
    int genuine_pts_count, interpolated_count, fallback_frames;
    int64_t last_video_pts_us;
    int video_disc;

    AVRational atb;
    int64_t first_audio_pts_us;
    int64_t last_audio_pts_us;
    int64_t last_decoded_audio_pts_us;
    int audio_disc, audio_missing_pts;
    int decoded_audio_frames;
    int64_t src_samples, out_samples;

    int frames_late;
    int late_40, late_80;
    int64_t offset_sum;
    int offset_n;
    int64_t offset_max_pos;       /* video ahead                         */
    int64_t offset_max_neg;       /* video behind (more negative)        */
    int64_t last_offset;
    int64_t first_offsets[OFFSET_LOG_N];
    int64_t last_offsets[OFFSET_LOG_N];
    int first_off_n, last_off_n;
    int pre_audio_frames;
} AVPlay;

static AVRational detect_fps(const AVPlay *p, const AVCodecContext *vdec)
{
    if (vdec && vdec->framerate.num > 0 && vdec->framerate.den > 0)
        return vdec->framerate;
    if (vdec && vdec->time_base.num > 0 && vdec->time_base.den > 0 &&
        vdec->ticks_per_frame > 0)
        return av_make_q(vdec->time_base.den,
                         vdec->time_base.num * vdec->ticks_per_frame);
    if (p->avg_frame_rate.num > 0 && p->avg_frame_rate.den > 0)
        return p->avg_frame_rate;
    if (p->r_frame_rate.num > 0 && p->r_frame_rate.den > 0)
        return p->r_frame_rate;
    return (AVRational){0, 0};
}

static int64_t frame_duration_tb(const AVPlay *p)
{
    if (p->fps.num > 0 && p->fps.den > 0 &&
        p->vtb.num > 0 && p->vtb.den > 0)
        return av_rescale_q(1, av_inv_q(p->fps), p->vtb);
    return 0;
}

static int64_t tb_to_us(AVRational tb, int64_t ticks)
{
    if (tb.num <= 0 || tb.den <= 0)
        return AV_NOPTS_VALUE;
    return av_rescale_q(ticks, tb, (AVRational){1, 1000000});
}

static int64_t audio_clock_us(AVPlay *p)
{
    if (p->first_audio_pts_us == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;
    if (p->mr->hw_pace)
        mraudio_poll(p->mr);
    return p->first_audio_pts_us + mraudio_elapsed_us(p->mr);
}

static void assign_video_pts(AVPlay *p, const AVFrame *frame,
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
            int64_t disc_us = tb_to_us(p->vtb, disc);
            if (disc_us < 0)
                disc_us = -disc_us;
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

static void wait_until(AVPlay *p, int64_t target)
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

static int64_t choose_present_vbl(AVPlay *p, int64_t pts_target, int64_t now,
                                  int first)
{
    int64_t min_k = p->present_vbl + 1;
    int64_t ideal = vbl_nearest(p->fb, pts_target);
    int64_t k = ideal;
    int64_t margin = p->fb->margin_us;
    if (k < min_k)
        k = min_k;
    while (vbl_time(p->fb, k) - margin < now)
        k++;
    (void)first;
    return k;
}

static void record_offset(AVPlay *p, int64_t off)
{
    if (p->first_off_n < OFFSET_LOG_N)
        p->first_offsets[p->first_off_n++] = off;
    if (p->last_off_n < OFFSET_LOG_N) {
        p->last_offsets[p->last_off_n++] = off;
    } else {
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

static int emit_pcm(AVPlay *p, uint8_t *chunk, int *used,
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
            mraudio_wait_fill(p->mr, WRITE_CHUNK);
            if (mraudio_write_all(p->mr, chunk, WRITE_CHUNK) < 0)
                return -1;
            *used = 0;
        }
    }
    return 0;
}

static AVCodecContext *open_decoder(AVStream *st, enum AVMediaType expect)
{
    AVCodecParameters *cp = st->codecpar;
    const AVCodec *codec;
    AVCodecContext *ctx;
    int r;
    stage = 4;
    if (expect == AVMEDIA_TYPE_VIDEO && cp->codec_id == AV_CODEC_ID_NONE)
        cp->codec_id = AV_CODEC_ID_MPEG2VIDEO;
    codec = avcodec_find_decoder(cp->codec_id);
    if (!codec) {
        fprintf(stderr, "No decoder for %s id %d (%s)\n",
                expect == AVMEDIA_TYPE_AUDIO ? "audio" : "video",
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

static int present_video_frame(AVPlay *p, AVFrame *frame, AVCodecContext *vdec)
{
    if (frame->width != FB_W || frame->height != FB_H) {
        fprintf(stderr, "FAIL: frame %d is %dx%d, expected %dx%d\n",
                p->rendered + 1, frame->width, frame->height, FB_W, FB_H);
        p->fail = 1;
        return -1;
    }

    if (!p->sws) {
        AVRational sar = frame->sample_aspect_ratio;
        AVRational fps0 = detect_fps(p, vdec);
        if (sar.num <= 0 || sar.den <= 0)
            sar = vdec->sample_aspect_ratio;
        if (sar.num <= 0 || sar.den <= 0)
            sar = (AVRational){1, 1};
        fprintf(stderr,
                "\n=== VIDEO PATH (mailbox, audio-master clock) ===\n"
                "Dimensions:          %dx%d  %s\n"
                "SAR:                 %d:%d\n"
                "Detected fps:        %d/%d (%.3f)\n"
                "HDMI vsync:          %.3f ms\n",
                frame->width, frame->height,
                av_get_pix_fmt_name(frame->format)
                    ? av_get_pix_fmt_name(frame->format) : "?",
                sar.num, sar.den, fps0.num, fps0.den,
                (fps0.num > 0 && fps0.den > 0) ? av_q2d(fps0) : 0.0,
                (double)p->fb->vsync_period_us / 1000.0);
        p->sws = sws_getContext(FB_W, FB_H, frame->format,
                                 FB_W, FB_H, AV_PIX_FMT_BGR0,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!p->sws) {
            fprintf(stderr, "Could not create swscale context\n");
            p->fail = 1;
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

    /* Hold early video until the hardware audio clock catches up. */
    if (p->mr->hw_pace && p->first_audio_pts_us != AV_NOPTS_VALUE &&
        vpts_us != AV_NOPTS_VALUE) {
        int64_t hold0 = av_gettime_relative();
        for (;;) {
            int64_t aclk = audio_clock_us(p);
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
    } else if (p->first_audio_pts_us == AV_NOPTS_VALUE) {
        p->pre_audio_frames++;
    }

    int next = p->displayed ^ 1;
    uint8_t *dst = next ? p->fb->fb_b : p->fb->fb_a;
    p->dst_data[0] = dst;
    p->dst_linesize[0] = FB_STRIDE;
    stage = 7;
    int64_t t0 = av_gettime_relative();
    sws_scale(p->sws, (const uint8_t * const *)frame->data, frame->linesize,
              0, FB_H, p->dst_data, p->dst_linesize);
    p->convert_us += av_gettime_relative() - t0;
    stage = 5;
    __sync_synchronize();

    int64_t now = av_gettime_relative();
    int64_t aclk = audio_clock_us(p);
    int64_t off = AV_NOPTS_VALUE;
    int64_t pts_target = now;

    if (aclk != AV_NOPTS_VALUE && vpts_us != AV_NOPTS_VALUE) {
        off = vpts_us - aclk;
        pts_target = now + off;
        record_offset(p, off);
    }

    int first = (p->rendered == 0);
    int64_t k = choose_present_vbl(p, pts_target, now, first);
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

static int decode_video_packet(AVPlay *p, AVCodecParserContext *parser,
                               AVCodecContext *vdec, AVPacket *ppkt,
                               AVFrame *frame, const uint8_t *in, int in_size,
                               int64_t in_pts, int64_t in_dts, int64_t in_pos)
{
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
            if (present_video_frame(p, frame, vdec) < 0)
                return -1;
            av_frame_unref(frame);
        }
    }
    return p->fail ? -1 : 0;
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
        if (dvdnav_open(&nav, cli.device) != DVDNAV_STATUS_OK) {
            fprintf(stderr, "dvdnav_open failed\n");
            return 1;
        }
        int lr = list_dvd_titles(nav);
        dvdnav_close(nav);
        return lr < 0 ? 1 : 0;
    }

    int64_t program_start = av_gettime_relative();
    fprintf(stderr, "=== SS1 DVD A/V PROOF (audio master clock) ===\n");
    fprintf(stderr, "FFmpeg CPU flags: 0x%x\n", av_get_cpu_flags());
    fprintf(stderr, "Leave OSD Buffer on A. ~%d s title 2 / chapter 1.\n",
            TARGET_SECONDS);

    av_log_set_level(AV_LOG_WARNING);

    FBPair fb;
    if (map_double_fb(&fb) < 0) {
        fprintf(stderr, "\nTEST ABORTED (DDR framebuffer not written).\n");
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
    fprintf(stderr, "Opening DVD %s...\n", cli.device);
    if (dvdnav_open(&d.nav, cli.device) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "dvdnav_open failed\n");
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
    if (dvdnav_get_number_of_titles(d.nav, &ntitles) != DVDNAV_STATUS_OK)
        ntitles = 0;
    fprintf(stderr, "Titles on disc: %d\n", (int)ntitles);
    if (jump_to_title(d.nav, &cli, ntitles) < 0)
        return 1;

    const AVInputFormat *mpeg = av_find_input_format("mpeg");
    if (!mpeg) {
        fprintf(stderr, "MPEG-PS demuxer missing\n");
        return 2;
    }

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

    AVPlay p;
    memset(&p, 0, sizeof(p));
    p.fb = &fb;
    p.mr = &mr;
    p.present_vbl = -1;
    p.first_genuine_pts = AV_NOPTS_VALUE;
    p.timeline_pts = AV_NOPTS_VALUE;
    p.assigned_pts = AV_NOPTS_VALUE;
    p.first_audio_pts_us = AV_NOPTS_VALUE;
    p.last_audio_pts_us = AV_NOPTS_VALUE;
    p.last_decoded_audio_pts_us = AV_NOPTS_VALUE;
    p.last_video_pts_us = AV_NOPTS_VALUE;

    AVPacket *pkt = av_packet_alloc();
    AVPacket *ppkt = av_packet_alloc();
    AVFrame *vframe = av_frame_alloc();
    AVFrame *aframe = av_frame_alloc();
    if (!pkt || !ppkt || !vframe || !aframe)
        return 6;

    AVCodecContext *vdec = NULL, *adec = NULL;
    AVCodecParserContext *parser = NULL;
    SwrContext *swr = NULL;
    AVStream *vst = NULL, *ast = NULL;
    int vi = -1, ai = -1;
    uint8_t **out_data = NULL;
    int out_linesize = 0, out_cap = 0;
    uint8_t chunk_buf[WRITE_CHUNK];
    int chunk_used = 0;
    unsigned long vpackets = 0, apackets = 0;
    int last_diag_s = -1;
    const int64_t target_elapsed = (int64_t)TARGET_SECONDS * 1000000;
    int64_t play_start = 0;
    int play_started = 0;

    fprintf(stderr, "\nReading DVD stream (A/V, ~%d s)...\n", TARGET_SECONDS);
    stage = 3;

    int read_ret = 0;
    while (!p.fail && mraudio_elapsed_us(&mr) < target_elapsed &&
           (read_ret = av_read_frame(fmt, pkt)) >= 0) {

        if (vi < 0 || ai < 0) {
            for (unsigned i = 0; i < fmt->nb_streams; ++i) {
                AVCodecParameters *cp = fmt->streams[i]->codecpar;
                if (vi < 0 && cp->codec_type == AVMEDIA_TYPE_VIDEO) {
                    vi = (int)i;
                    vst = fmt->streams[i];
                    if (cp->codec_id == AV_CODEC_ID_NONE)
                        cp->codec_id = AV_CODEC_ID_MPEG2VIDEO;
                    fprintf(stderr, "Video stream #%d  %s\n",
                            vi, avcodec_get_name(cp->codec_id));
                    vdec = open_decoder(vst, AVMEDIA_TYPE_VIDEO);
                    if (!vdec)
                        return 7;
                    parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
                    if (!parser) {
                        fprintf(stderr, "Could not init MPEG-2 parser\n");
                        return 7;
                    }
                    p.vtb = vst->time_base;
                    p.avg_frame_rate = vst->avg_frame_rate;
                    p.r_frame_rate = vst->r_frame_rate;
                    fprintf(stderr, "Video timebase: %d/%d\n",
                            p.vtb.num, p.vtb.den);
                }
                if (ai < 0 && cp->codec_type == AVMEDIA_TYPE_AUDIO) {
                    char layout[128] = "unknown";
                    ai = (int)i;
                    ast = fmt->streams[i];
                    if (cp->ch_layout.nb_channels > 0)
                        av_channel_layout_describe(&cp->ch_layout,
                                                   layout, sizeof(layout));
                    fprintf(stderr,
                            "Audio stream #%d  %s  %d Hz  %d ch  %s  tb %d/%d\n",
                            ai, avcodec_get_name(cp->codec_id),
                            cp->sample_rate, cp->ch_layout.nb_channels, layout,
                            ast->time_base.num, ast->time_base.den);
                    adec = open_decoder(ast, AVMEDIA_TYPE_AUDIO);
                    if (!adec)
                        return 7;
                    p.atb = ast->time_base;
                }
            }
        }

        if (ai >= 0 && pkt->stream_index == ai && adec) {
            apackets++;
            stage = 5;
            if (avcodec_send_packet(adec, pkt) >= 0) {
                while (avcodec_receive_frame(adec, aframe) == 0) {
                    p.decoded_audio_frames++;
                    p.src_samples += aframe->nb_samples;
                    if (aframe->pts == AV_NOPTS_VALUE) {
                        p.audio_missing_pts++;
                    } else {
                        int64_t pus = tb_to_us(p.atb, aframe->pts);
                        if (p.first_audio_pts_us == AV_NOPTS_VALUE &&
                            pus != AV_NOPTS_VALUE) {
                            p.first_audio_pts_us = pus;
                            fprintf(stderr,
                                    "First decoded audio PTS: %" PRId64
                                    "  (%.6f s)  [audio clock origin]\n",
                                    aframe->pts, pus / 1e6);
                        }
                        if (p.last_audio_pts_us != AV_NOPTS_VALUE &&
                            pus != AV_NOPTS_VALUE) {
                            int64_t gap = pus - p.last_audio_pts_us;
                            if (gap < 0)
                                gap = -gap;
                            if (gap > DISC_JUMP_US)
                                p.audio_disc++;
                        }
                        p.last_audio_pts_us = pus;
                        p.last_decoded_audio_pts_us = pus;
                    }
                    if (!swr) {
                        AVChannelLayout out_layout;
                        av_channel_layout_default(&out_layout, OUT_CHANNELS);
                        if (swr_alloc_set_opts2(
                                &swr, &out_layout, AV_SAMPLE_FMT_S16, OUT_RATE,
                                &aframe->ch_layout,
                                (enum AVSampleFormat)aframe->format,
                                aframe->sample_rate, 0, NULL) < 0 ||
                            !swr || swr_init(swr) < 0) {
                            fprintf(stderr, "swr init failed\n");
                            return 7;
                        }
                        av_channel_layout_uninit(&out_layout);
                        fprintf(stderr,
                                "Audio convert: %s %d Hz %d ch -> 48 kHz S16 stereo\n",
                                av_get_sample_fmt_name(aframe->format)
                                    ? av_get_sample_fmt_name(aframe->format)
                                    : "?",
                                aframe->sample_rate,
                                aframe->ch_layout.nb_channels);
                    }
                    {
                        int max_out = (int)av_rescale_rnd(
                            swr_get_delay(swr, aframe->sample_rate) +
                                aframe->nb_samples,
                            OUT_RATE, aframe->sample_rate, AV_ROUND_UP);
                        if (max_out < 32)
                            max_out = 32;
                        if (max_out > out_cap) {
                            if (out_data)
                                av_freep(&out_data[0]);
                            av_freep(&out_data);
                            if (av_samples_alloc_array_and_samples(
                                    &out_data, &out_linesize, OUT_CHANNELS,
                                    max_out, AV_SAMPLE_FMT_S16, 0) < 0)
                                return 6;
                            out_cap = max_out;
                        }
                        int got = swr_convert(
                            swr, out_data, max_out,
                            (const uint8_t **)aframe->extended_data,
                            aframe->nb_samples);
                        if (got > 0) {
                            p.out_samples += got;
                            if (!play_started) {
                                play_start = av_gettime_relative();
                                play_started = 1;
                            }
                            if (emit_pcm(&p, chunk_buf, &chunk_used,
                                         out_data[0], got * OUT_BYTES) < 0)
                                return 8;
                        }
                    }
                    av_frame_unref(aframe);
                }
            }
        } else if (vi >= 0 && pkt->stream_index == vi && vdec && parser) {
            vpackets++;
            if (decode_video_packet(&p, parser, vdec, ppkt, vframe,
                                    pkt->data, pkt->size,
                                    pkt->pts, pkt->dts, pkt->pos) < 0)
                return 10;
        }

        av_packet_unref(pkt);

        {
            int64_t el = mraudio_elapsed_us(&mr);
            int sec = (int)(el / 1000000);
            if (sec != last_diag_s && (sec % 2) == 0 && sec > 0) {
                last_diag_s = sec;
                int64_t aclk = audio_clock_us(&p);
                fprintf(stderr,
                        "  t=%ds  aclk=%.3fs  apts=%.3fs  vpts=%.3fs  "
                        "v-a=%+.1fms  fill=%d (%.0fms)  frames=%d  late=%d\n",
                        sec,
                        aclk == AV_NOPTS_VALUE ? 0.0 : aclk / 1e6,
                        p.last_decoded_audio_pts_us == AV_NOPTS_VALUE
                            ? 0.0 : p.last_decoded_audio_pts_us / 1e6,
                        p.last_video_pts_us == AV_NOPTS_VALUE
                            ? 0.0 : p.last_video_pts_us / 1e6,
                        p.last_offset / 1000.0,
                        mr.fill,
                        mr.fill * 1000.0 / (double)BYTES_PER_SEC,
                        p.rendered, p.frames_late);
            }
        }
    }

    if (chunk_used > 0) {
        while (chunk_used & 3)
            chunk_buf[chunk_used++] = 0;
        mraudio_wait_fill(&mr, chunk_used);
        if (mraudio_write_all(&mr, chunk_buf, chunk_used) < 0)
            return 8;
    }

    fprintf(stderr, "Draining MrAudio ring...\n");
    mraudio_drain(&mr);
    if (mr.hw_pace)
        mraudio_poll(&mr);

    int64_t play_end = av_gettime_relative();
    double wall = play_started
                  ? (double)(play_end - play_start) / 1e6
                  : (double)(play_end - program_start) / 1e6;
    int64_t consumed = mraudio_consumed(&mr);
    double hw_dur = (double)consumed / (double)BYTES_PER_SEC;
    double fill_avg = mr.fill_n ? (double)mr.fill_sum / mr.fill_n : 0.0;
    double off_avg = p.offset_n ? (double)p.offset_sum / p.offset_n : 0.0;

    fprintf(stderr,
            "\n=== A/V REPORT ===\n"
            "Wall playback duration:     %8.3f s\n"
            "Hardware audio consumed:    %8.3f s  (%" PRId64 " bytes)\n"
            "Audio samples written:      %" PRId64 "  (%.3f s)\n"
            "Audio samples consumed:     %" PRId64 "\n"
            "Prime bytes excluded:       %" PRId64 "\n"
            "MrAudio fill min/avg/max:   %" PRId64 " / %.0f / %" PRId64 " bytes  "
            "(%.1f / %.1f / %.1f ms)\n"
            "Video frames displayed:     %d  (A=%d B=%d)\n"
            "Demuxed video/audio pkts:   %lu / %lu\n"
            "Pre-audio-clock frames:     %d\n"
            "Average video-audio offset: %+.2f ms\n"
            "Max video ahead:            %+.2f ms\n"
            "Max video behind:           %+.2f ms\n"
            "Frames >40 ms late:         %d\n"
            "Frames >80 ms late:         %d\n"
            "Video frames late (<0):     %d\n"
            "Audio PTS discontinuities:  %d\n"
            "Video PTS discontinuities:  %d\n"
            "Pacing:                     %s\n",
            wall, hw_dur, consumed,
            p.out_samples, (double)p.out_samples / (double)OUT_RATE,
            consumed / OUT_BYTES,
            mr.prime_bytes,
            mr.fill_min, fill_avg, mr.fill_max,
            mr.fill_min * 1000.0 / BYTES_PER_SEC,
            fill_avg * 1000.0 / BYTES_PER_SEC,
            mr.fill_max * 1000.0 / BYTES_PER_SEC,
            p.rendered, p.frames_a, p.frames_b,
            vpackets, apackets,
            p.pre_audio_frames,
            off_avg / 1000.0,
            (double)p.offset_max_pos / 1000.0,
            (double)p.offset_max_neg / 1000.0,
            p.late_40, p.late_80, p.frames_late,
            p.audio_disc, p.video_disc,
            mr.hw_pace ? "hardware FPGA rptr/len" : "DEGRADED wall-clock");

    fprintf(stderr, "A/V offset near start (ms):");
    for (int i = 0; i < p.first_off_n; i++)
        fprintf(stderr, "  %+.1f", p.first_offsets[i] / 1000.0);
    fprintf(stderr, "%s\n", p.first_off_n ? "" : "  (none)");
    fprintf(stderr, "A/V offset near end (ms):  ");
    for (int i = 0; i < p.last_off_n; i++)
        fprintf(stderr, "  %+.1f", p.last_offsets[i] / 1000.0);
    fprintf(stderr, "%s\n", p.last_off_n ? "" : "  (none)");

    if (read_ret < 0 && mraudio_elapsed_us(&mr) < target_elapsed) {
        char e[128];
        fferr(read_ret, e, sizeof(e));
        fprintf(stderr, "Demux ended early: %s\n", e);
    }

    mraudio_close(&mr);
    unmap_double_fb(&fb);
    if (out_data)
        av_freep(&out_data[0]);
    av_freep(&out_data);
    swr_free(&swr);
    if (p.sws)
        sws_freeContext(p.sws);
    av_frame_free(&vframe);
    av_frame_free(&aframe);
    av_packet_free(&pkt);
    av_packet_free(&ppkt);
    if (parser)
        av_parser_close(parser);
    avcodec_free_context(&vdec);
    avcodec_free_context(&adec);
    avformat_close_input(&fmt);
    avio_context_free(&avio);
    dvdnav_close(d.nav);
    free(d.sector);

    if (p.fail)
        return 10;
    if (p.rendered <= 0 || p.out_samples <= 0)
        return 9;

    fprintf(stderr,
            "\nPASS: ~%.1f s A/V through mailbox + /dev/MrAudio "
            "(audio-master proof, not the finished player).\n",
            hw_dur);
    return 0;
}
