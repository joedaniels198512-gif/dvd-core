/*
 * dvd_launcher.c — first public-facing SS1 DVD launcher.
 *
 * Renders into the same DDR A/B + mailbox path as dvd_av_threaded_test
 * (0x30000000 / 0x30200000 / 0x30400000). Does not mmap /dev/fb0 pixels.
 * Invokes the hardware-tested player as a child process and redraws when
 * playback exits.
 */

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#define _GNU_SOURCE

#include "dvd_library.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

#define FB_A_PHYS       0x30000000UL
#define FB_B_PHYS       0x30200000UL
#define MB_PHYS         0x30400000UL
#define JOY_OFF         8
#define JOY_MAGIC       0x44564431u
#define DISP_BUF_BIT    31
#define JOY_BTN_MASK    0x3FFu
#define JOY_BIT_RIGHT   0
#define JOY_BIT_LEFT    1
#define JOY_BIT_DOWN    2
#define JOY_BIT_UP      3
#define JOY_BIT_SELECT  4
#define JOY_BIT_BACK    5

#define FB_W            720
#define FB_H            576
#define UI_H            480
#define FB_STRIDE       2880
#define FB_SIZE         ((size_t)FB_STRIDE * FB_H)
#define MB_MAP_SIZE     4096

#define CTRL_POLL_US          8000
#define CTRL_REPEAT_DELAY_US  400000
#define CTRL_REPEAT_RATE_US   120000

#define ISO_DIR         "/media/fat/DVD/isos"
#define SR0_PATH        "/dev/sr0"
#define LIB_VIS         7
#define LAUNCHER_PID_FILE "/tmp/dvd_launcher.pid"

#define COL_BG          0x00000000u
#define COL_TEXT        0x00E0E0E0u
#define COL_DIM         0x00909090u
#define COL_ACCENT      0x0000E0E0u /* BGR0: yellow */
#define COL_BAR         0x00303000u
#define COL_ARROW       0x0000FFFFu

#define FONT_H          8
#define FONT_W          8

enum {
    SCR_MAIN = 0,
    SCR_LIBRARY,
    SCR_ERROR
};

typedef struct {
    uint8_t *fb_a, *fb_b;
    volatile uint32_t *mbox;
    int mem_fd, fb_fd;
} FBPair;

typedef struct {
    uint32_t prev_bits;
    int primed;
    int64_t dir_held_us[2];
    int64_t dir_last_us[2];
} Pad;

static volatile sig_atomic_t g_stop = 0;
static pid_t g_child = 0;

/* 8x8 glyphs for ASCII 32..126. Row-major, MSB (bit 7) = leftmost pixel. */
static const uint8_t font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00},
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
    {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00},
    {0x38,0x6C,0x38,0x70,0xDE,0xCC,0x76,0x00},
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    {0x02,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00},
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00},
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00},
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00},
    {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00},
    {0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0x00},
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, /* A */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00},
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00},
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0x00},
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00},
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00},
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00},
    {0x3C,0x66,0x66,0x66,0x76,0x6C,0x36,0x00},
    {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00},
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00},
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    {0x40,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x18,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, /* a */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00},
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00},
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00},
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    {0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0x00},
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C},
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00},
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    {0x06,0x00,0x06,0x06,0x06,0x06,0x66,0x3C},
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x00,0x00,0xEC,0xFE,0xD6,0xC6,0xC6,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},
    {0x00,0x00,0x6C,0x76,0x60,0x60,0x60,0x00},
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    {0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},
    {0x00,0x00,0xC6,0xD6,0xFE,0x7C,0x6C,0x00},
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C},
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00},
    {0x0C,0x18,0x18,0x70,0x18,0x18,0x0C,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    {0x30,0x18,0x18,0x0E,0x18,0x18,0x30,0x00},
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
};

static int64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_child > 0)
        kill(g_child, SIGTERM);
}

static void write_launcher_pid(void)
{
    FILE *f = fopen(LAUNCHER_PID_FILE, "w");

    if (!f)
        return;
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
}

static void clear_launcher_pid(void)
{
    unlink(LAUNCHER_PID_FILE);
}

static void install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int map_fb(FBPair *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->mem_fd = -1;
    fb->fb_fd = -1;
    fb->mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fb->mem_fd < 0) {
        fprintf(stderr, "open /dev/mem: %s (need root)\n", strerror(errno));
        return -1;
    }
    fcntl(fb->mem_fd, F_SETFD, FD_CLOEXEC);
    fb->fb_a = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fb->mem_fd, (off_t)FB_A_PHYS);
    fb->fb_b = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fb->mem_fd, (off_t)FB_B_PHYS);
    fb->mbox = mmap(NULL, MB_MAP_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fb->mem_fd, (off_t)MB_PHYS);
    if (fb->fb_a == MAP_FAILED || fb->fb_b == MAP_FAILED ||
        fb->mbox == MAP_FAILED) {
        fprintf(stderr, "mmap DDR/mailbox failed: %s\n", strerror(errno));
        return -1;
    }
    fb->fb_fd = open("/dev/fb0", O_RDWR);
    if (fb->fb_fd >= 0)
        fcntl(fb->fb_fd, F_SETFD, FD_CLOEXEC);
    return 0;
}

static void unmap_fb(FBPair *fb)
{
    if (fb->fb_a && fb->fb_a != MAP_FAILED)
        munmap(fb->fb_a, FB_SIZE);
    if (fb->fb_b && fb->fb_b != MAP_FAILED)
        munmap(fb->fb_b, FB_SIZE);
    if (fb->mbox && fb->mbox != MAP_FAILED)
        munmap((void *)fb->mbox, MB_MAP_SIZE);
    if (fb->mem_fd >= 0)
        close(fb->mem_fd);
    if (fb->fb_fd >= 0)
        close(fb->fb_fd);
}

static void wait_vsync(const FBPair *fb)
{
    int arg = 0;

    if (fb->fb_fd >= 0 && ioctl(fb->fb_fd, FBIO_WAITFORVSYNC, &arg) == 0)
        return;
    usleep(20000);
}

static void put_pixel(uint8_t *dst, int x, int y, uint32_t pix)
{
    if (x < 0 || y < 0 || x >= FB_W || y >= UI_H)
        return;
    *(uint32_t *)(dst + (size_t)y * FB_STRIDE + (size_t)x * 4) = pix;
}

static void fill_rect(uint8_t *dst, int x, int y, int w, int h, uint32_t pix)
{
    int i, j;

    if (w < 1 || h < 1)
        return;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            put_pixel(dst, x + i, y + j, pix);
}

static void draw_char(uint8_t *dst, int x, int y, char ch, int scale,
                      uint32_t pix)
{
    const uint8_t *g;
    int gx, gy, sx, sy;
    unsigned idx;

    if (ch < 32 || ch > 126)
        ch = '?';
    idx = (unsigned)(ch - 32);
    g = font8x8[idx];
    if (scale < 2)
        scale = 2;
    for (gy = 0; gy < FONT_H; gy++) {
        uint8_t row = g[gy];
        for (gx = 0; gx < FONT_W; gx++) {
            if (row & (0x80u >> gx)) {
                for (sy = 0; sy < scale; sy++)
                    for (sx = 0; sx < scale; sx++)
                        put_pixel(dst, x + gx * scale + sx,
                                  y + gy * scale + sy, pix);
            }
        }
    }
}

static int text_width(const char *s, int scale)
{
    if (scale < 2)
        scale = 2;
    return (int)strlen(s) * FONT_W * scale;
}

static void draw_text(uint8_t *dst, int x, int y, const char *s, int scale,
                      uint32_t pix)
{
    int cx = x;

    if (scale < 2)
        scale = 2;
    for (; s && *s; s++) {
        if (cx + FONT_W * scale > FB_W - 8)
            break;
        draw_char(dst, cx, y, *s, scale, pix);
        cx += FONT_W * scale;
    }
}

static void draw_text_max(uint8_t *dst, int x, int y, const char *s, int scale,
                          uint32_t pix, int max_x)
{
    int cx = x;

    if (scale < 2)
        scale = 2;
    for (; s && *s; s++) {
        if (cx + FONT_W * scale > max_x)
            break;
        draw_char(dst, cx, y, *s, scale, pix);
        cx += FONT_W * scale;
    }
}

static void draw_text_centered(uint8_t *dst, int y, const char *s, int scale,
                               uint32_t pix)
{
    int w = text_width(s, scale);
    int x = (FB_W - w) / 2;

    if (x < 16)
        x = 16;
    draw_text(dst, x, y, s, scale, pix);
}

static void clear_ui(uint8_t *dst)
{
    memset(dst, 0, FB_SIZE);
}

static void render_main(uint8_t *dst, int sel)
{
    int y0 = 168;
    int y1 = 216;
    int bar_y = sel ? y1 : y0;

    clear_ui(dst);
    draw_text_centered(dst, 48, "MiSTer DVD Player", 4, COL_TEXT);
    draw_text_centered(dst, 96, "by Mojojojo198512", 2, COL_DIM);

    fill_rect(dst, 80, bar_y - 8, FB_W - 160, 40, COL_BAR);
    fill_rect(dst, 80, bar_y - 8, 8, 40, COL_ACCENT);
    draw_text(dst, 108, y0, "Play Physical DVD", 2,
              sel == 0 ? COL_ARROW : COL_TEXT);
    draw_text(dst, 108, y1, "DVD Library", 2,
              sel == 1 ? COL_ARROW : COL_TEXT);
    if (sel == 0)
        draw_text(dst, 84, y0, ">", 2, COL_ARROW);
    else
        draw_text(dst, 84, y1, ">", 2, COL_ARROW);

    draw_text_centered(dst, 368, "Beta build", 2, COL_DIM);
    draw_text_centered(dst, 408, "Use only media you are authorised to access",
                       2, COL_DIM);
}

static void render_scan(uint8_t *dst)
{
    clear_ui(dst);
    draw_text_centered(dst, 200, "Scanning DVD Library...", 2, COL_TEXT);
}

static void render_library(uint8_t *dst, const DvdLibItem *items, int n, int sel)
{
    int i, first, pages, page, y;
    char pagebuf[32];

    clear_ui(dst);
    draw_text_centered(dst, 28, "DVD LIBRARY", 3, COL_TEXT);

    if (n <= 0) {
        draw_text_centered(dst, 140, "No DVD-Video ISOs found", 2, COL_TEXT);
        draw_text_centered(dst, 196, "Put DVD ISOs in:", 2, COL_DIM);
        draw_text_centered(dst, 232, ISO_DIR, 2, COL_DIM);
        draw_text_centered(dst, 268, "or USB /DVD /Movies", 2, COL_DIM);
        draw_text_centered(dst, 420, "B: Back", 2, COL_DIM);
        return;
    }

    pages = (n + LIB_VIS - 1) / LIB_VIS;
    page = sel / LIB_VIS;
    first = page * LIB_VIS;

    y = 96;
    for (i = first; i < n && i < first + LIB_VIS; i++) {
        if (i == sel) {
            fill_rect(dst, 40, y - 6, FB_W - 80, 32, COL_BAR);
            fill_rect(dst, 40, y - 6, 8, 32, COL_ACCENT);
            draw_text(dst, 56, y, ">", 2, COL_ARROW);
        }
        draw_text_max(dst, 88, y, items[i].display, 2,
                      i == sel ? COL_ARROW : COL_TEXT, 600);
        draw_text(dst, 620, y,
                  items[i].source == DVD_LIB_SD ? "SD" : "USB", 2, COL_DIM);
        y += 36;
    }
    snprintf(pagebuf, sizeof(pagebuf), "Page %d/%d", page + 1, pages);
    draw_text_centered(dst, 380, pagebuf, 2, COL_DIM);
    draw_text_centered(dst, 420, "Confirm to play   B: Back", 2, COL_DIM);
}

static void render_error(uint8_t *dst, const char *title, const char *msg)
{
    clear_ui(dst);
    draw_text_centered(dst, 160, title && title[0] ? title : "Unable to read DVD",
                       3, COL_TEXT);
    if (msg && msg[0])
        draw_text_centered(dst, 220, msg, 2, COL_DIM);
    draw_text_centered(dst, 320, "Press B to return", 2, COL_ARROW);
}

static void present(FBPair *fb, void (*draw)(uint8_t *, void *), void *arg)
{
    draw(fb->fb_a, arg);
    memcpy(fb->fb_b, fb->fb_a, FB_SIZE);
    __sync_synchronize();
    fb->mbox[0] = 0;
    wait_vsync(fb);
}

struct DrawMain { int sel; };
struct DrawLib { const DvdLibItem *items; int n, sel; };
struct DrawErr { const char *title; const char *msg; };
struct DrawScan { int unused; };

static void draw_main_cb(uint8_t *d, void *arg)
{
    struct DrawMain *a = arg;
    render_main(d, a->sel);
}

static void draw_lib_cb(uint8_t *d, void *arg)
{
    struct DrawLib *a = arg;
    render_library(d, a->items, a->n, a->sel);
}

static void draw_err_cb(uint8_t *d, void *arg)
{
    struct DrawErr *a = arg;
    render_error(d, a->title, a->msg);
}

static void draw_scan_cb(uint8_t *d, void *arg)
{
    (void)arg;
    render_scan(d);
}

static int read_buttons(const FBPair *fb, uint32_t *out)
{
    volatile uint64_t *st =
        (volatile uint64_t *)((uint8_t *)fb->mbox + JOY_OFF);
    uint64_t w = *st;
    uint32_t magic = (uint32_t)(w >> 32);
    uint32_t joy = (uint32_t)w;

    if (magic != JOY_MAGIC)
        return -1;
    *out = joy & JOY_BTN_MASK;
    return 0;
}

enum {
    ACT_NONE = 0,
    ACT_UP,
    ACT_DOWN,
    ACT_CONFIRM,
    ACT_CANCEL
};

static int pad_poll(Pad *pad, uint32_t bits, int64_t t)
{
    int up = !!(bits & (1u << JOY_BIT_UP));
    int down = !!(bits & (1u << JOY_BIT_DOWN));
    int confirm = !!(bits & (1u << JOY_BIT_SELECT));
    int cancel = !!(bits & (1u << JOY_BIT_BACK));
    int prev_up, prev_down, prev_c, prev_b;
    int act = ACT_NONE;

    if (!pad->primed) {
        pad->prev_bits = bits;
        pad->primed = 1;
        return ACT_NONE;
    }
    prev_up = !!(pad->prev_bits & (1u << JOY_BIT_UP));
    prev_down = !!(pad->prev_bits & (1u << JOY_BIT_DOWN));
    prev_c = !!(pad->prev_bits & (1u << JOY_BIT_SELECT));
    prev_b = !!(pad->prev_bits & (1u << JOY_BIT_BACK));

    if (up && !prev_up)
        act = ACT_UP;
    else if (down && !prev_down)
        act = ACT_DOWN;
    else if (confirm && !prev_c)
        act = ACT_CONFIRM;
    else if (cancel && !prev_b)
        act = ACT_CANCEL;

    {
        int dirs[2] = { up, down };
        int acts[2] = { ACT_UP, ACT_DOWN };
        int i;
        for (i = 0; i < 2; i++) {
            if (dirs[i]) {
                if (!pad->dir_held_us[i]) {
                    pad->dir_held_us[i] = t;
                    pad->dir_last_us[i] = t;
                } else if (t - pad->dir_held_us[i] >= CTRL_REPEAT_DELAY_US &&
                           t - pad->dir_last_us[i] >= CTRL_REPEAT_RATE_US) {
                    if (act == ACT_NONE)
                        act = acts[i];
                    pad->dir_last_us[i] = t;
                }
            } else {
                pad->dir_held_us[i] = 0;
                pad->dir_last_us[i] = 0;
            }
        }
    }
    pad->prev_bits = bits;
    return act;
}

static int resolve_player(char *out, size_t cap, const char *argv0)
{
    char dir[512];
    const char *slash;
    size_t n;

    if (!argv0 || !*argv0)
        argv0 = ".";
    slash = strrchr(argv0, '/');
    if (slash) {
        n = (size_t)(slash - argv0);
        if (n >= sizeof(dir))
            n = sizeof(dir) - 1;
        memcpy(dir, argv0, n);
        dir[n] = 0;
    } else {
        strcpy(dir, ".");
    }
    snprintf(out, cap, "%s/dvd_av_threaded_test", dir);
    if (access(out, X_OK) == 0)
        return 0;
    snprintf(out, cap, "./dvd_av_threaded_test");
    if (access(out, X_OK) == 0)
        return 0;
    fprintf(stderr, "LAUNCHER: player not found next to launcher\n");
    return -1;
}

static int disc_present(void)
{
    int fd;

    if (access(SR0_PATH, F_OK) != 0)
        return 0;
    fd = open(SR0_PATH, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static int run_player(const char *player, const char *source)
{
    pid_t pid;
    int status = 0;

    fprintf(stderr, "LAUNCHER: starting %s\n", source);
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "LAUNCHER: fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl(player, "dvd_av_threaded_test", source,
              "--buffered-yuv-video",
              "--initial-video-skip", "1",
              "--video-advance-ms", "20",
              "--authored-start",
              (char *)NULL);
        fprintf(stderr, "LAUNCHER: exec failed: %s\n", strerror(errno));
        _exit(127);
    }
    g_child = pid;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            g_child = 0;
            return -1;
        }
    }
    g_child = 0;
    if (WIFEXITED(status)) {
        fprintf(stderr, "LAUNCHER: player exited (%d)\n", WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "LAUNCHER: player killed by signal %d\n",
                WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    fprintf(stderr, "LAUNCHER: player exited\n");
    return -1;
}

int main(int argc, char **argv)
{
    FBPair fb;
    Pad pad;
    char player[512];
    DvdLibItem library[DVD_LIB_MAX_ITEMS];
    DvdLibStats lib_stats;
    int n_lib = 0;
    int screen = SCR_MAIN;
    int main_sel = 0;
    int lib_sel = 0;
    int dirty = 1;
    int err_back = SCR_MAIN;
    const char *err_title = "Unable to read DVD";
    const char *err_msg = "";

    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    install_signals();
    memset(&pad, 0, sizeof(pad));
    memset(library, 0, sizeof(library));

    if (argc >= 2 && !strcmp(argv[1], "--scan-dump")) {
        n_lib = dvd_library_refresh(library, DVD_LIB_MAX_ITEMS, &lib_stats);
        return 0;
    }

    if (resolve_player(player, sizeof(player), argc > 0 ? argv[0] : NULL) < 0)
        return 1;
    if (map_fb(&fb) < 0)
        return 1;
    write_launcher_pid();

    fprintf(stderr, "MiSTer DVD Player launcher ready\n");

    while (!g_stop) {
        uint32_t bits = 0;
        int act = ACT_NONE;
        int64_t t = now_us();

        if (dirty) {
            if (screen == SCR_MAIN) {
                struct DrawMain a = { main_sel };
                present(&fb, draw_main_cb, &a);
            } else if (screen == SCR_LIBRARY) {
                struct DrawLib a = { library, n_lib, lib_sel };
                present(&fb, draw_lib_cb, &a);
            } else {
                struct DrawErr a = { err_title, err_msg };
                present(&fb, draw_err_cb, &a);
            }
            dirty = 0;
        }

        if (read_buttons(&fb, &bits) == 0)
            act = pad_poll(&pad, bits, t);

        if (screen == SCR_MAIN) {
            if (act == ACT_UP && main_sel > 0) {
                main_sel--;
                dirty = 1;
            } else if (act == ACT_DOWN && main_sel < 1) {
                main_sel++;
                dirty = 1;
            } else if (act == ACT_CONFIRM) {
                if (main_sel == 0) {
                    int rc;
                    fprintf(stderr, "LAUNCHER: Play Physical DVD\n");
                    if (!disc_present()) {
                        err_title = "Unable to read DVD";
                        err_msg = "No disc in drive";
                        err_back = SCR_MAIN;
                        screen = SCR_ERROR;
                        pad.primed = 0;
                        dirty = 1;
                    } else {
                        rc = run_player(player, SR0_PATH);
                        pad.primed = 0;
                        if (g_stop)
                            break;
                        if (rc == 1 || rc == 127) {
                            err_title = "Unable to read DVD";
                            err_msg = "Disc could not be opened";
                            err_back = SCR_MAIN;
                            screen = SCR_ERROR;
                        } else {
                            screen = SCR_MAIN;
                        }
                        dirty = 1;
                    }
                } else {
                    struct DrawScan scan = { 0 };
                    fprintf(stderr, "LAUNCHER: DVD Library\n");
                    present(&fb, draw_scan_cb, &scan);
                    n_lib = dvd_library_refresh(library, DVD_LIB_MAX_ITEMS,
                                                &lib_stats);
                    lib_sel = 0;
                    screen = SCR_LIBRARY;
                    pad.primed = 0;
                    dirty = 1;
                }
            }
        } else if (screen == SCR_LIBRARY) {
            if (act == ACT_CANCEL) {
                screen = SCR_MAIN;
                pad.primed = 0;
                dirty = 1;
            } else if (n_lib > 0 && act == ACT_UP && lib_sel > 0) {
                lib_sel--;
                dirty = 1;
            } else if (n_lib > 0 && act == ACT_DOWN && lib_sel < n_lib - 1) {
                lib_sel++;
                dirty = 1;
            } else if (n_lib > 0 && act == ACT_CONFIRM) {
                int rc;
                struct stat stbuf;

                if (stat(library[lib_sel].path, &stbuf) != 0) {
                    err_title = "DVD no longer available";
                    err_msg = "";
                    err_back = SCR_LIBRARY;
                    screen = SCR_ERROR;
                    pad.primed = 0;
                    dirty = 1;
                } else {
                    fprintf(stderr, "LAUNCHER: DVD Library play %s\n",
                            library[lib_sel].path);
                    rc = run_player(player, library[lib_sel].path);
                    pad.primed = 0;
                    if (g_stop)
                        break;
                    if (rc == 1 || rc == 127) {
                        err_title = "Unable to read DVD";
                        err_msg = "File could not be opened";
                        err_back = SCR_LIBRARY;
                        screen = SCR_ERROR;
                    } else {
                        screen = SCR_LIBRARY;
                    }
                    dirty = 1;
                }
            }
        } else if (screen == SCR_ERROR) {
            if (act == ACT_CANCEL) {
                screen = err_back;
                pad.primed = 0;
                dirty = 1;
            }
        }

        usleep(CTRL_POLL_US);
    }

    unmap_fb(&fb);
    clear_launcher_pid();
    if (g_stop)
        fprintf(stderr, "LAUNCHER: stopped by SIGTERM/SIGINT\n");
    fprintf(stderr, "LAUNCHER: exit\n");
    return 0;
}
