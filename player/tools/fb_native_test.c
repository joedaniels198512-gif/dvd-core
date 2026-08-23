/*
 * fb_native_test.c — minimal SS1/MiSTer HPS-framebuffer hardware experiment.
 *
 * Proves that the existing MiSTer framework (Main_MiSTer + MiSTer_fb kernel
 * module + ASCAL) can display an ARM-written native 720x576 BGR0 image with
 * ALL scaling done by the FPGA.
 *
 * Mechanism (no FPGA changes, no Main_MiSTer changes):
 *   1. "fb_cmd1 8888 1 720 576" -> /dev/MiSTer_cmd
 *      Main_MiSTer sends UIO_SET_FBUF (0x2F) to the framework and reprograms
 *      the MiSTer_fb kernel module, so /dev/fb0 becomes a native 720x576
 *      XRGB8888 (BGR0) buffer that ASCAL reads and upscales.
 *      (See Main_MiSTer/video.cpp video_cmd(), "fb_cmd1 %d %d %d %d".)
 *   2. Draw a test pattern at native resolution. No software scaling.
 *   3. FBIO_WAITFORVSYNC (real HDMI vsync IRQ from the FPGA) is benchmarked.
 *   4. The previous framebuffer geometry (read from
 *      /sys/module/MiSTer_fb/parameters/mode before the test) is re-applied.
 *
 * RESTORATION CAVEAT: /dev/MiSTer_cmd has no "framebuffer off" command, so
 * returning to *core video* is not possible from userspace. Run this from the
 * MiSTer menu core; there the restored geometry brings the console/menu back.
 * Inside a game core the screen stays in framebuffer mode until the core is
 * reloaded or the OSD is used.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FB_DEV       "/dev/fb0"
#define CMD_DEV      "/dev/MiSTer_cmd"
#define MODE_SYSFS   "/sys/module/MiSTer_fb/parameters/mode"

#define TEST_W       720
#define TEST_H       576
#define VSYNC_SAMPLES 300
#define DISPLAY_SECONDS 10

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/*
 * Send one command line to Main_MiSTer's command FIFO.
 */
static int mister_cmd(const char *cmd)
{
    int fd = open(CMD_DEV, O_WRONLY);

    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", CMD_DEV, strerror(errno));
        return -1;
    }

    char line[128];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);

    int ok = (write(fd, line, (size_t)n) == n) ? 0 : -1;

    if (ok < 0)
        fprintf(stderr, "write %s: %s\n", CMD_DEV, strerror(errno));

    close(fd);
    return ok;
}

/*
 * Read the current MiSTer_fb geometry: "format rb width height stride".
 * Returns 0 and fills the 5 values on success.
 */
static int read_fb_mode(int *fmt, int *rb, int *w, int *h, int *stride)
{
    FILE *fp = fopen(MODE_SYSFS, "r");

    if (!fp)
        return -1;

    int n = fscanf(fp, "%d %d %d %d %d", fmt, rb, w, h, stride);

    fclose(fp);
    return (n == 5) ? 0 : -1;
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * XRGB8888 with red at bit 16 (== BGR0 byte order in memory),
 * exactly what "fb_cmd1 8888 1 ..." configures.
 */
static inline uint32_t px(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void draw_pattern(uint8_t *mem, uint32_t stride, int w, int h)
{
    /* Classic 8-column colour bars. */
    static const uint8_t bars[8][3] = {
        { 255, 255, 255 }, /* white   */
        { 255, 255,   0 }, /* yellow  */
        {   0, 255, 255 }, /* cyan    */
        {   0, 255,   0 }, /* green   */
        { 255,   0, 255 }, /* magenta */
        { 255,   0,   0 }, /* red     */
        {   0,   0, 255 }, /* blue    */
        {  32,  32,  32 }, /* near-black */
    };

    const int border = 4;

    const int cx = w / 2;
    const int cy = h / 2;
    const int radius = h / 4;          /* 144 px on 576 lines */
    const int ring = 5;

    const int r_out2 = (radius + ring) * (radius + ring);
    const int r_in2  = (radius - ring) * (radius - ring);

    for (int y = 0; y < h; y++) {

        uint32_t *row = (uint32_t *)(mem + (size_t)y * stride);

        for (int x = 0; x < w; x++) {

            uint32_t c;

            int bar = (x * 8) / w;
            c = px(bars[bar][0], bars[bar][1], bars[bar][2]);

            /* Centered white ring on top of the bars. */
            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;

            if (d2 <= r_out2 && d2 >= r_in2)
                c = px(255, 255, 255);

            /* White frame border: proves no edge is cropped. */
            if (x < border || y < border ||
                x >= w - border || y >= h - border)
                c = px(255, 255, 255);

            row[x] = c;
        }
    }
}

static void benchmark_vsync(int fd)
{
    uint32_t arg = 0;

    /* Probe support first. */
    if (ioctl(fd, FBIO_WAITFORVSYNC, &arg) < 0) {
        fprintf(stderr,
                "FBIO_WAITFORVSYNC not working (%s) - skipping benchmark.\n",
                strerror(errno));
        return;
    }

    fprintf(stderr, "Measuring %d vsync waits...\n", VSYNC_SAMPLES);

    int timeouts = 0;
    double t0 = now_sec();

    for (int i = 0; i < VSYNC_SAMPLES; i++) {
        arg = 0;
        if (ioctl(fd, FBIO_WAITFORVSYNC, &arg) < 0) {
            if (errno == ETIMEDOUT) {
                timeouts++;
                continue;
            }
            fprintf(stderr, "vsync wait failed at %d: %s\n",
                    i, strerror(errno));
            return;
        }
    }

    double total = now_sec() - t0;
    double avg_ms = (total / VSYNC_SAMPLES) * 1000.0;

    fprintf(stderr,
            "\n=== VSYNC RESULTS ===\n"
            "Waits:            %d (%d timeouts)\n"
            "Total duration:   %.3f s\n"
            "Average interval: %.3f ms\n"
            "Measured refresh: %.3f Hz\n\n",
            VSYNC_SAMPLES, timeouts,
            total, avg_ms,
            (total > 0.0) ? (VSYNC_SAMPLES / total) : 0.0);
}

int main(void)
{
    fprintf(stderr, "=== SS1 NATIVE HPS FRAMEBUFFER TEST (720x576) ===\n");

    /* 1. Preconditions. */
    if (!file_exists(CMD_DEV)) {
        fprintf(stderr, "FAIL: %s missing - Main_MiSTer not running?\n",
                CMD_DEV);
        return 1;
    }
    if (!file_exists(FB_DEV)) {
        fprintf(stderr, "FAIL: %s missing - MiSTer_fb module not loaded?\n",
                FB_DEV);
        return 1;
    }

    /* Save previous geometry for restoration. */
    int old_fmt, old_rb, old_w, old_h, old_stride;
    int have_old = (read_fb_mode(&old_fmt, &old_rb,
                                 &old_w, &old_h, &old_stride) == 0);

    if (have_old)
        fprintf(stderr,
                "Previous fb mode: fmt=%d rb=%d %dx%d stride=%d\n",
                old_fmt, old_rb, old_w, old_h, old_stride);
    else
        fprintf(stderr,
                "WARNING: could not read %s - restoration unavailable.\n",
                MODE_SYSFS);

    /* 2. Ask Main_MiSTer for a native 720x576 BGR0 framebuffer. */
    fprintf(stderr, "Sending: fb_cmd1 8888 1 %d %d\n", TEST_W, TEST_H);

    if (mister_cmd("fb_cmd1 8888 1 720 576") < 0)
        return 2;

    /* Main processes the FIFO asynchronously and the kernel fb re-registers;
       give it a moment before touching /dev/fb0. */
    usleep(500 * 1000);

    /* 3. Query the result. */
    int fd = open(FB_DEV, O_RDWR);

    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", FB_DEV, strerror(errno));
        return 3;
    }

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;

    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0 ||
        ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {

        fprintf(stderr, "FBIOGET_*SCREENINFO: %s\n", strerror(errno));
        close(fd);
        return 3;
    }

    fprintf(stderr,
            "\nFramebuffer after fb_cmd1:\n"
            "  xres x yres : %u x %u\n"
            "  bpp         : %u\n"
            "  stride      : %u bytes\n"
            "  red offset  : %u (len %u)\n"
            "  green offset: %u (len %u)\n"
            "  blue offset : %u (len %u)\n"
            "  phys base   : 0x%lx\n",
            vinfo.xres, vinfo.yres,
            vinfo.bits_per_pixel,
            finfo.line_length,
            vinfo.red.offset, vinfo.red.length,
            vinfo.green.offset, vinfo.green.length,
            vinfo.blue.offset, vinfo.blue.length,
            finfo.smem_start);

    int failed = 0;

    if (vinfo.xres != TEST_W || vinfo.yres != TEST_H) {
        fprintf(stderr,
                "WARNING: geometry is not %dx%d. Main_MiSTer clamps the size\n"
                "to the current output mode - use a 720p/1080p HDMI mode.\n",
                TEST_W, TEST_H);
        failed = 1;
    }

    if (vinfo.bits_per_pixel != 32 ||
        vinfo.red.offset != 16 ||
        vinfo.green.offset != 8 ||
        vinfo.blue.offset != 0) {

        fprintf(stderr, "WARNING: pixel format is not BGR0/XRGB8888.\n");
        failed = 1;
    }

    /* 4. Map and 5. draw (at whatever geometry we actually got, so the
       pattern is still visible for diagnosis even when clamped). */
    size_t map_len = (size_t)finfo.line_length * vinfo.yres;

    uint8_t *mem = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);

    if (mem == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        close(fd);
        return 4;
    }

    double t_draw = now_sec();
    draw_pattern(mem, finfo.line_length, (int)vinfo.xres, (int)vinfo.yres);
    fprintf(stderr, "Pattern drawn in %.2f ms (%ux%u, native, no scaling).\n",
            (now_sec() - t_draw) * 1000.0, vinfo.xres, vinfo.yres);

    /* 8. Vsync benchmark (~5-6 s of the display window). */
    double t_shown = now_sec();
    benchmark_vsync(fd);

    /* 7. Keep the image up for ~10 s total. */
    double remain = DISPLAY_SECONDS - (now_sec() - t_shown);
    if (remain > 0) {
        fprintf(stderr, "Holding image for another %.1f s...\n", remain);
        usleep((useconds_t)(remain * 1e6));
    }

    munmap(mem, map_len);
    close(fd);

    /* 9. Restore previous geometry (best effort - see file header). */
    if (have_old) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "fb_cmd1 %d %d %d %d",
                 old_fmt, old_rb, old_w, old_h);
        fprintf(stderr, "Restoring previous fb mode: %s\n", cmd);
        mister_cmd(cmd);
        fprintf(stderr,
                "NOTE: there is no public 'framebuffer off' command. If this\n"
                "was run inside a game core (not the menu), reload the core\n"
                "to return to core video.\n");
    }

    if (failed) {
        fprintf(stderr, "\nDONE WITH WARNINGS (see above).\n");
        return 5;
    }

    fprintf(stderr,
            "\nPASS: native 720x576 image was displayed via the MiSTer\n"
            "HPS framebuffer with FPGA (ASCAL) scaling.\n");
    return 0;
}
