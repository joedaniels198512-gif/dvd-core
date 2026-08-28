/* SPDX-License-Identifier: GPL-2.0-or-later */
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
#include <strings.h>
#include <sys/file.h>
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
#define JOY_BTN_MASK    0xFFFu
#define JOY_BIT_RIGHT   0
#define JOY_BIT_LEFT    1
#define JOY_BIT_DOWN    2
#define JOY_BIT_UP      3
#define JOY_BIT_SELECT  4  /* J1[0] CONFIRM */
#define JOY_BIT_BACK    5  /* J1[1] CANCEL */
#define JOY_BIT_PLAYPAUSE 6 /* J1[2] Start */
#define JOY_BIT_MENU    7  /* J1[3] DVD Menu */
#define JOY_BIT_PREV    8  /* J1[4] L */
#define JOY_BIT_NEXT    9  /* J1[5] R */
#define JOY_BIT_SUBTITLE   10 /* J1[6] Y — player-only, launcher just logs */
#define JOY_BIT_AUDIO_NEXT 11 /* J1[7] Select — player-only, launcher just logs */

#define CHEAT_TIMEOUT_US     3000000
#define KUN_PERIOD_US        62500   /* 16 fps */
#define KUN_ART_W            16
#define KUN_ART_H            16
#define KUN_SCALE            3
#define KUN_W                (KUN_ART_W * KUN_SCALE)
#define KUN_H                (KUN_ART_H * KUN_SCALE)

#define FB_W            720
#define FB_H            576
#define UI_H            480
#define FB_STRIDE       2880
#define FB_SIZE         ((size_t)FB_STRIDE * FB_H)
#define MB_MAP_SIZE     4096

#define CTRL_POLL_US          8000
#define CTRL_REPEAT_DELAY_US  400000
#define CTRL_REPEAT_RATE_US   120000

enum {
    CHEAT_NONE = 0,
    CHEAT_UP,
    CHEAT_DOWN,
    CHEAT_LEFT,
    CHEAT_RIGHT,
    CHEAT_OTHER
};

#define DVD_PLAYER_VERSION "0.1.0-private-beta"
#define SR0_PATH        "/dev/sr0"
#define ISO_DIR         "/media/fat/DVD/isos"
#define LIB_VIS         7
#define LAUNCHER_PID_FILE "/tmp/dvd_launcher.pid"

enum {
    MODE_LAUNCHER_ACTIVE = 0,
    MODE_PLAYER_ACTIVE
};

static volatile sig_atomic_t g_stop = 0;
static pid_t g_child = 0;
static int g_launcher_mode = MODE_LAUNCHER_ACTIVE;
static int g_pid_fd = -1;
static uint32_t g_last_input_bits = 0xffffffffu;

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
    SCR_ERROR,
    SCR_RIP_PREP,
    SCR_RIP_USB,
    SCR_RIP_RUN,
    SCR_RIP_CANCEL,
    SCR_RIP_DONE,
    SCR_RIP_FAIL
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

typedef struct {
    int enabled;
    int x, y;
    int vx, vy;
    int64_t last_us;
} KunState;

typedef struct {
    int pos;
    int64_t last_us;
} CheatState;

static KunState g_kun;
static CheatState g_cheat;

typedef struct {
    int x, y;
    int valid;
} KunBufState;

typedef struct {
    int x, y, w, h;
} Rect;

static KunBufState g_kun_buf[2];
static int g_mbox_req;
static uint8_t g_ui_cache[FB_STRIDE * UI_H];
static int g_ui_cache_sel = -1;
static int g_full_redraws;
static uint32_t g_mascot_bytes;
static int g_mascot_frames;
static int64_t g_mascot_dbg_t0;

/* Temporary placeholder mascot (no local MiSTer-kun asset in the repo).
 * ' ' transparent, '#' outline, 'o' body, '*' accent, '.' eyes. */
static const char kun_art[KUN_ART_H][KUN_ART_W + 1] = {
    "                ",
    "      ****      ",
    "    **oooo**    ",
    "   *oooooooo*   ",
    "  #oooooooooo#  ",
    "  #oo.#  #.oo#  ",
    "  #oooooooooo#  ",
    "  #oooooooooo#  ",
    "   #oooooooo#   ",
    "    ########    ",
    "     #    #     ",
    "    # oooo #    ",
    "    #oooooo#    ",
    "     #oooo#     ",
    "      #oo#      ",
    "       ##       ",
};

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

static int read_buttons(const FBPair *fb, uint32_t *out);

static const char *launcher_mode_name(int mode)
{
    return mode == MODE_PLAYER_ACTIVE ? "PLAYER_ACTIVE" : "LAUNCHER_ACTIVE";
}

static void set_launcher_mode(int mode, const char *why)
{
    g_launcher_mode = mode;
    fprintf(stderr, "launcher_mode=%s%s%s\n",
            launcher_mode_name(mode),
            (why && why[0]) ? " why=" : "",
            (why && why[0]) ? why : "");
}

static void log_launcher_input(uint32_t bits)
{
    if (bits == g_last_input_bits)
        return;
    g_last_input_bits = bits;
    fprintf(stderr, "LAUNCHER INPUT raw=0x%03x launcher_mode=%s\n",
            bits & JOY_BTN_MASK, launcher_mode_name(g_launcher_mode));
}

static void log_launcher_action(const char *action)
{
    fprintf(stderr, "LAUNCHER ACTION %s launcher_mode=%s\n",
            action ? action : "?", launcher_mode_name(g_launcher_mode));
}

static int wait_controller_neutral(const FBPair *fb)
{
    uint32_t bits = 0;
    int64_t t0 = now_us();

    while (!g_stop) {
        bits = 0;
        if (read_buttons(fb, &bits) != 0 || bits == 0)
            break;
        if (now_us() - t0 > 5000000)
            break;
        usleep(CTRL_POLL_US);
    }
    fprintf(stderr, "controller neutral raw=0x%03x\n", bits & JOY_BTN_MASK);
    return 0;
}

static int cmdline_is_player(pid_t pid)
{
    char path[64];
    char buf[256];
    int fd, n;

    if (pid <= 0)
        return 0;
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    return strstr(buf, "dvd_av_threaded_test") != NULL;
}

static int player_process_running(void)
{
    DIR *d;
    struct dirent *e;
    int found = 0;

    d = opendir("/proc");
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        pid_t pid;
        char *end = NULL;

        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        pid = (pid_t)strtol(e->d_name, &end, 10);
        if (!end || *end || pid <= 0 || pid == getpid())
            continue;
        if (cmdline_is_player(pid)) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int claim_launcher_singleton(void)
{
    char buf[32];
    int n;

    g_pid_fd = open(LAUNCHER_PID_FILE, O_RDWR | O_CREAT, 0644);
    if (g_pid_fd < 0) {
        fprintf(stderr, "LAUNCHER: pid file open failed: %s\n", strerror(errno));
        return -1;
    }
    if (flock(g_pid_fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "LAUNCHER: another instance is running — exit\n");
        close(g_pid_fd);
        g_pid_fd = -1;
        return -1;
    }
    (void)ftruncate(g_pid_fd, 0);
    n = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    if (n > 0)
        (void)write(g_pid_fd, buf, (size_t)n);
    return 0;
}

static void clear_launcher_pid(void)
{
    if (g_pid_fd >= 0) {
        flock(g_pid_fd, LOCK_UN);
        close(g_pid_fd);
        g_pid_fd = -1;
    }
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

static uint32_t kun_pix(char ch)
{
    switch (ch) {
    case '#':
        return 0x00202020u;
    case 'o':
        return COL_TEXT;
    case '*':
        return COL_ACCENT;
    case '.':
        return COL_ARROW;
    default:
        return 0;
    }
}

static void kun_init(void)
{
    g_kun.enabled = 0;
    g_kun.x = 36;
    g_kun.y = 300;
    g_kun.vx = 3;
    g_kun.vy = 2;
    g_kun.last_us = 0;
    g_cheat.pos = 0;
    g_cheat.last_us = 0;
    g_kun_buf[0].valid = 0;
    g_kun_buf[1].valid = 0;
    g_mbox_req = 0;
    g_ui_cache_sel = -1;
}

static void kun_step(void)
{
    g_kun.x += g_kun.vx;
    g_kun.y += g_kun.vy;
    if (g_kun.x <= 0) {
        g_kun.x = 0;
        g_kun.vx = -g_kun.vx;
    } else if (g_kun.x + KUN_W >= FB_W) {
        g_kun.x = FB_W - KUN_W;
        g_kun.vx = -g_kun.vx;
    }
    if (g_kun.y <= 0) {
        g_kun.y = 0;
        g_kun.vy = -g_kun.vy;
    } else if (g_kun.y + KUN_H >= UI_H) {
        g_kun.y = UI_H - KUN_H;
        g_kun.vy = -g_kun.vy;
    }
}

static int launcher_debug(void)
{
    const char *e = getenv("DVD_LAUNCHER_DEBUG");

    return e && e[0] && e[0] != '0';
}

static int clamp_rect(Rect *r)
{
    if (r->x < 0) {
        r->w += r->x;
        r->x = 0;
    }
    if (r->y < 0) {
        r->h += r->y;
        r->y = 0;
    }
    if (r->x + r->w > FB_W)
        r->w = FB_W - r->x;
    if (r->y + r->h > UI_H)
        r->h = UI_H - r->y;
    return r->w > 0 && r->h > 0;
}

static int rects_overlap(Rect a, Rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

static Rect merge_rect(Rect a, Rect b)
{
    Rect r;
    int x2, y2;

    r.x = a.x < b.x ? a.x : b.x;
    r.y = a.y < b.y ? a.y : b.y;
    x2 = (a.x + a.w > b.x + b.w) ? a.x + a.w : b.x + b.w;
    y2 = (a.y + a.h > b.y + b.h) ? a.y + a.h : b.y + b.h;
    r.w = x2 - r.x;
    r.h = y2 - r.y;
    return r;
}

static uint32_t cache_pixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= FB_W || y >= UI_H)
        return 0xffffffffu;
    return *(uint32_t *)(g_ui_cache + (size_t)y * FB_STRIDE + (size_t)x * 4);
}

static size_t blit_cache_rect(uint8_t *dst, Rect r)
{
    int row;
    size_t bytes = 0;
    size_t rowb;

    if (!clamp_rect(&r))
        return 0;
    rowb = (size_t)r.w * 4;
    for (row = 0; row < r.h; row++) {
        memcpy(dst + (size_t)(r.y + row) * FB_STRIDE + (size_t)r.x * 4,
               g_ui_cache + (size_t)(r.y + row) * FB_STRIDE + (size_t)r.x * 4,
               rowb);
        bytes += rowb;
    }
    return bytes;
}

static void draw_kun_behind(uint8_t *dst, int x0, int y0)
{
    int ay, ax, sy, sx, x, y;
    uint32_t pix;

    for (ay = 0; ay < KUN_ART_H; ay++) {
        for (ax = 0; ax < KUN_ART_W; ax++) {
            pix = kun_pix(kun_art[ay][ax]);
            if (!pix)
                continue;
            for (sy = 0; sy < KUN_SCALE; sy++) {
                for (sx = 0; sx < KUN_SCALE; sx++) {
                    x = x0 + ax * KUN_SCALE + sx;
                    y = y0 + ay * KUN_SCALE + sy;
                    if (!cache_pixel(x, y))
                        put_pixel(dst, x, y, pix);
                }
            }
        }
    }
}

static int read_display_buf(const FBPair *fb, int *out)
{
    volatile uint64_t *st =
        (volatile uint64_t *)((uint8_t *)fb->mbox + JOY_OFF);
    uint64_t w = *st;
    uint32_t magic = (uint32_t)(w >> 32);
    uint32_t joy = (uint32_t)w;

    if (magic != JOY_MAGIC)
        return -1;
    *out = (int)((joy >> DISP_BUF_BIT) & 1u);
    return 0;
}

static void mascot_dbg_tick(size_t bytes)
{
    int64_t now, dt;
    int fps;

    g_mascot_bytes += (uint32_t)bytes;
    g_mascot_frames++;
    if (!launcher_debug())
        return;
    now = now_us();
    if (!g_mascot_dbg_t0)
        g_mascot_dbg_t0 = now;
    dt = now - g_mascot_dbg_t0;
    if (dt < 1000000)
        return;
    fps = (int)((g_mascot_frames * 1000000LL) / dt);
    fprintf(stderr,
            "MASCOT: dirty bytes/frame=%u  fps=%d  full_redraws=%d\n",
            g_mascot_frames ? (g_mascot_bytes / (uint32_t)g_mascot_frames) : 0,
            fps, g_full_redraws);
    g_mascot_bytes = 0;
    g_mascot_frames = 0;
    g_full_redraws = 0;
    g_mascot_dbg_t0 = now;
}

static void mascot_present_dirty(FBPair *fb)
{
    int shown = g_mbox_req;
    int dest;
    uint8_t *ddr;
    Rect nw;
    size_t bytes = 0;

    if (read_display_buf(fb, &shown) != 0)
        shown = g_mbox_req;
    dest = shown ^ 1;
    ddr = dest ? fb->fb_b : fb->fb_a;
    nw.x = g_kun.x;
    nw.y = g_kun.y;
    nw.w = KUN_W;
    nw.h = KUN_H;
    if (g_kun_buf[dest].valid) {
        Rect old;

        old.x = g_kun_buf[dest].x;
        old.y = g_kun_buf[dest].y;
        old.w = KUN_W;
        old.h = KUN_H;
        if (rects_overlap(old, nw))
            bytes += blit_cache_rect(ddr, merge_rect(old, nw));
        else {
            bytes += blit_cache_rect(ddr, old);
            bytes += blit_cache_rect(ddr, nw);
        }
    } else {
        bytes += blit_cache_rect(ddr, nw);
    }
    draw_kun_behind(ddr, g_kun.x, g_kun.y);
    g_kun_buf[dest].x = g_kun.x;
    g_kun_buf[dest].y = g_kun.y;
    g_kun_buf[dest].valid = 1;
    __sync_synchronize();
    fb->mbox[0] = (uint32_t)dest;
    g_mbox_req = dest;
    mascot_dbg_tick(bytes);
}

static int cheat_rising(uint32_t bits, uint32_t prev)
{
    if ((bits & (1u << JOY_BIT_UP)) && !(prev & (1u << JOY_BIT_UP)))
        return CHEAT_UP;
    if ((bits & (1u << JOY_BIT_DOWN)) && !(prev & (1u << JOY_BIT_DOWN)))
        return CHEAT_DOWN;
    if ((bits & (1u << JOY_BIT_LEFT)) && !(prev & (1u << JOY_BIT_LEFT)))
        return CHEAT_LEFT;
    if ((bits & (1u << JOY_BIT_RIGHT)) && !(prev & (1u << JOY_BIT_RIGHT)))
        return CHEAT_RIGHT;
    if ((bits & (1u << JOY_BIT_SELECT)) && !(prev & (1u << JOY_BIT_SELECT)))
        return CHEAT_OTHER;
    if ((bits & (1u << JOY_BIT_BACK)) && !(prev & (1u << JOY_BIT_BACK)))
        return CHEAT_OTHER;
    if ((bits & (1u << JOY_BIT_PLAYPAUSE)) &&
        !(prev & (1u << JOY_BIT_PLAYPAUSE)))
        return CHEAT_OTHER;
    if ((bits & (1u << JOY_BIT_MENU)) && !(prev & (1u << JOY_BIT_MENU)))
        return CHEAT_OTHER;
    if ((bits & (1u << JOY_BIT_PREV)) && !(prev & (1u << JOY_BIT_PREV)))
        return CHEAT_OTHER;
    if ((bits & (1u << JOY_BIT_NEXT)) && !(prev & (1u << JOY_BIT_NEXT)))
        return CHEAT_OTHER;
    return CHEAT_NONE;
}

static void cheat_reset(void)
{
    g_cheat.pos = 0;
}

static int cheat_feed(int btn, int64_t t)
{
    static const int seq[] = {
        CHEAT_UP, CHEAT_UP, CHEAT_DOWN, CHEAT_DOWN,
        CHEAT_LEFT, CHEAT_RIGHT, CHEAT_LEFT, CHEAT_RIGHT
    };

    if (btn == CHEAT_NONE)
        return 0;
    g_cheat.last_us = t;
    if (btn == seq[g_cheat.pos]) {
        g_cheat.pos++;
        if (g_cheat.pos == 8) {
            g_kun.enabled = !g_kun.enabled;
            g_cheat.pos = 0;
            g_kun.last_us = t;
            fprintf(stderr, g_kun.enabled ?
                    "LAUNCHER: easter egg enabled\n" :
                    "LAUNCHER: easter egg disabled\n");
            return 1;
        }
        return 0;
    }
    if (btn == seq[0])
        g_cheat.pos = 1;
    else
        g_cheat.pos = 0;
    return 0;
}

static int cheat_poll(const Pad *pad, uint32_t bits, int64_t t)
{
    int btn;

    if (!pad->primed)
        return 0;
    if (g_cheat.pos && t - g_cheat.last_us > CHEAT_TIMEOUT_US)
        cheat_reset();
    btn = cheat_rising(bits, pad->prev_bits);
    if (btn == CHEAT_NONE)
        return 0;
    return cheat_feed(btn, t);
}

static void render_main(uint8_t *dst, int sel)
{
    static const char *items[3] = {
        "Play Physical DVD",
        "DVD Library",
        "Rip DVD to USB"
    };
    int i;
    int y0 = 152;

    draw_text_centered(dst, 40, "MiSTer DVD Player", 4, COL_TEXT);
    draw_text_centered(dst, 88, "by Mojojojo198512", 2, COL_DIM);

    for (i = 0; i < 3; i++) {
        int y = y0 + i * 44;
        if (i == sel) {
            fill_rect(dst, 80, y - 8, FB_W - 160, 40, COL_BAR);
            fill_rect(dst, 80, y - 8, 8, 40, COL_ACCENT);
            draw_text(dst, 84, y, ">", 2, COL_ARROW);
        }
        draw_text(dst, 108, y, items[i], 2,
                  i == sel ? COL_ARROW : COL_TEXT);
    }

    draw_text_centered(dst, 300, "To exit DVD, hold BACK for 3 seconds",
                       2, COL_DIM);
    draw_text_centered(dst, 348, "Beta 0.1.0", 2, COL_DIM);
    draw_text_centered(dst, 388, "Use only media you are authorised to access",
                       2, COL_DIM);
}

static void rebuild_ui_cache(int sel)
{
    memset(g_ui_cache, 0, sizeof(g_ui_cache));
    render_main(g_ui_cache, sel);
    g_ui_cache_sel = sel;
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

#define RIP_STATUS_PATH "/tmp/dvd_rip_status"

typedef struct {
    char helper[512];
    char title[80];
    char file[96];
    char vol[40];
    char size_s[24];
    char required_s[24];
    char usb_path[256];
    char usb_free[24];
    char usb_fs[32];
    char usb_list[8][64];
    char usb_paths[8][256];
    char usb_fstypes[8][32];
    uint64_t usb_freeb[8];
    uint64_t usb_avail;
    int nusb;
    int usb_sel;
    int prep_sel;
    int cancel_sel;
    int done_sel;
    int pct;
    char msg[80];
    char fail[96];
    char done_name[96];
    uint64_t bytes;
    pid_t pid;
    int running;
} RipUi;

static RipUi g_rip;

static void rip_reset(void)
{
    memset(&g_rip, 0, sizeof(g_rip));
}

static int resolve_rip_helper(char *out, size_t cap, const char *argv0)
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
    snprintf(out, cap, "%s/dvd_rip_iso", dir);
    if (access(out, X_OK) == 0)
        return 0;
    snprintf(out, cap, "./dvd_rip_iso");
    return access(out, X_OK) == 0 ? 0 : -1;
}

static int rip_kv(const char *line, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p, *e;

    snprintf(pat, sizeof(pat), "%s=", key);
    p = strstr(line, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    e = strchr(p, '|');
    if (!e)
        e = p + strlen(p);
    while (e > p && (e[-1] == '\n' || e[-1] == '\r'))
        e--;
    if ((size_t)(e - p) >= cap)
        return 0;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return 1;
}

static int rip_read_cmd(const char *helper, char *const argv[],
                        char *out, size_t cap)
{
    int pfd[2];
    pid_t pid;
    ssize_t n, o = 0;
    int status;

    if (pipe(pfd) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execv(helper, argv);
        _exit(127);
    }
    close(pfd[1]);
    while (o + 1 < (ssize_t)cap) {
        n = read(pfd[0], out + o, cap - 1 - (size_t)o);
        if (n <= 0)
            break;
        o += n;
    }
    out[o] = 0;
    close(pfd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int rip_probe(const char *helper)
{
    char *av[] = { (char *)helper, "probe", SR0_PATH, NULL };
    char buf[1024];
    int rc;
    char bytes[32];

    rc = rip_read_cmd(helper, av, buf, sizeof(buf));
    if (rc != 0 || !strstr(buf, "probe=ok"))
        return rc == 6 ? 6 : 10;
    rip_kv(buf, "title", g_rip.title, sizeof(g_rip.title));
    rip_kv(buf, "file", g_rip.file, sizeof(g_rip.file));
    rip_kv(buf, "vol", g_rip.vol, sizeof(g_rip.vol));
    rip_kv(buf, "size", g_rip.size_s, sizeof(g_rip.size_s));
    if (rip_kv(buf, "bytes", bytes, sizeof(bytes)))
        g_rip.bytes = strtoull(bytes, NULL, 10);
    if (!g_rip.title[0])
        snprintf(g_rip.title, sizeof(g_rip.title), "DVD");
    if (!g_rip.file[0])
        snprintf(g_rip.file, sizeof(g_rip.file), "%s.iso", g_rip.title);
    return 0;
}

static int rip_list_usb(const char *helper)
{
    char *av[] = { (char *)helper, "list-usb", NULL };
    char buf[2048], *line, *save;
    int rc;

    g_rip.nusb = 0;
    rc = rip_read_cmd(helper, av, buf, sizeof(buf));
    (void)rc;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char path[256], freeb[32], fs[32], wr[8], name[64];
        const char *slash;
        if (!strstr(line, "usb=") || strstr(line, "usb=none"))
            continue;
        if (g_rip.nusb >= 8)
            break;
        path[0] = freeb[0] = fs[0] = wr[0] = 0;
        rip_kv(line, "path", path, sizeof(path));
        rip_kv(line, "free", freeb, sizeof(freeb));
        rip_kv(line, "fstype", fs, sizeof(fs));
        rip_kv(line, "writable", wr, sizeof(wr));
        if (!path[0] || wr[0] == '0')
            continue;
        snprintf(g_rip.usb_paths[g_rip.nusb], sizeof(g_rip.usb_paths[0]),
                 "%s", path);
        snprintf(g_rip.usb_fstypes[g_rip.nusb], sizeof(g_rip.usb_fstypes[0]),
                 "%s", fs);
        g_rip.usb_freeb[g_rip.nusb] = strtoull(freeb, NULL, 10);
        slash = strrchr(path, '/');
        snprintf(name, sizeof(name), "%s  %s", slash ? slash + 1 : path, fs);
        snprintf(g_rip.usb_list[g_rip.nusb], sizeof(g_rip.usb_list[0]),
                 "%s", name);
        g_rip.nusb++;
    }
    return g_rip.nusb;
}

static uint64_t rip_required_bytes(void)
{
    return g_rip.bytes * 2ULL + 512ULL * 1024ULL * 1024ULL;
}

static void rip_compute_required(void)
{
    uint64_t req = rip_required_bytes();
    double gb = req / (1024.0 * 1024.0 * 1024.0);

    snprintf(g_rip.required_s, sizeof(g_rip.required_s),
             gb >= 10 ? "%.0f GB" : "%.1f GB", gb);
}

static void rip_apply_usb(int i)
{
    double gb;

    if (i < 0 || i >= g_rip.nusb)
        return;
    g_rip.usb_sel = i;
    snprintf(g_rip.usb_path, sizeof(g_rip.usb_path), "%s",
             g_rip.usb_paths[i]);
    snprintf(g_rip.usb_fs, sizeof(g_rip.usb_fs), "%s",
             g_rip.usb_fstypes[i]);
    g_rip.usb_avail = g_rip.usb_freeb[i];
    gb = g_rip.usb_avail / (1024.0 * 1024.0 * 1024.0);
    snprintf(g_rip.usb_free, sizeof(g_rip.usb_free),
             gb >= 10 ? "%.0f GB" : "%.1f GB", gb);
}

static int rip_fs_is_fat32(const char *fs)
{
    return fs && (!strcasecmp(fs, "vfat") || !strcasecmp(fs, "msdos") ||
                  !strcasecmp(fs, "fat") || !strcasecmp(fs, "fat32"));
}

static int rip_preflight(char *why, size_t cap)
{
    char path[512];
    struct stat st;

    if (rip_fs_is_fat32(g_rip.usb_fs) &&
        g_rip.bytes > (4ULL * 1024ULL * 1024ULL * 1024ULL - 1ULL)) {
        snprintf(why, cap, "This DVD is too large for FAT32. Use an exFAT USB drive.");
        return 4;
    }
    if (g_rip.usb_avail < rip_required_bytes()) {
        snprintf(why, cap, "Not enough free space to create ISO.");
        return 5;
    }
    snprintf(path, sizeof(path), "%s/DVD/%s", g_rip.usb_path, g_rip.file);
    if (stat(path, &st) == 0) {
        snprintf(why, cap, "ISO already exists");
        return 3;
    }
    return 0;
}

static void rip_map_fail(int rc)
{
    if (g_rip.fail[0])
        return;
    switch (rc) {
    case 2:
        snprintf(g_rip.fail, sizeof(g_rip.fail), "Cancelled");
        break;
    case 3:
        snprintf(g_rip.fail, sizeof(g_rip.fail), "ISO already exists");
        break;
    case 4:
        snprintf(g_rip.fail, sizeof(g_rip.fail),
                 "This DVD is too large for FAT32. Use an exFAT USB drive.");
        break;
    case 5:
        snprintf(g_rip.fail, sizeof(g_rip.fail),
                 "Not enough free space to create ISO.");
        break;
    case 6:
        snprintf(g_rip.fail, sizeof(g_rip.fail), "No disc in drive");
        break;
    case 7:
        snprintf(g_rip.fail, sizeof(g_rip.fail), "Disc read error");
        break;
    case 8:
        snprintf(g_rip.fail, sizeof(g_rip.fail), "ISO validation failed");
        break;
    case 9:
        snprintf(g_rip.fail, sizeof(g_rip.fail), "No USB drive found");
        break;
    case 10:
        snprintf(g_rip.fail, sizeof(g_rip.fail), "Not a DVD-Video disc");
        break;
    default:
        snprintf(g_rip.fail, sizeof(g_rip.fail), "Rip failed");
        break;
    }
}

static int rip_enter(const char *helper)
{
    int rc;

    rip_reset();
    snprintf(g_rip.helper, sizeof(g_rip.helper), "%s", helper);
    rc = rip_probe(helper);
    if (rc != 0) {
        rip_map_fail(rc);
        return -1;
    }
    rip_compute_required();
    if (rip_list_usb(helper) <= 0) {
        snprintf(g_rip.fail, sizeof(g_rip.fail), "No USB drive found");
        return -1;
    }
    if (g_rip.nusb == 1) {
        rip_apply_usb(0);
        return SCR_RIP_PREP;
    }
    return SCR_RIP_USB;
}

static void render_rip_prep(uint8_t *dst)
{
    char line[96];
    int y0 = 300, y1 = 340;
    int sel = g_rip.prep_sel;
    int yb = sel ? y1 : y0;

    clear_ui(dst);
    draw_text_centered(dst, 28, "Rip DVD", 3, COL_TEXT);
    draw_text_centered(dst, 80, g_rip.title, 2, COL_ARROW);
    snprintf(line, sizeof(line), "Disc size:   %s", g_rip.size_s);
    draw_text(dst, 120, 140, line, 2, COL_TEXT);
    snprintf(line, sizeof(line), "USB free:    %s", g_rip.usb_free);
    draw_text(dst, 120, 176, line, 2, COL_TEXT);
    snprintf(line, sizeof(line), "Required:    %s", g_rip.required_s);
    draw_text(dst, 120, 212, line, 2, COL_TEXT);
    draw_text_centered(dst, 252, "Copy only media you are authorised to copy.",
                       1, COL_DIM);
    fill_rect(dst, 80, yb - 8, FB_W - 160, 40, COL_BAR);
    fill_rect(dst, 80, yb - 8, 8, 40, COL_ACCENT);
    draw_text(dst, 84, y0, sel == 0 ? ">" : " ", 2, COL_ARROW);
    draw_text(dst, 108, y0, "Start Rip", 2, sel == 0 ? COL_ARROW : COL_TEXT);
    draw_text(dst, 84, y1, sel == 1 ? ">" : " ", 2, COL_ARROW);
    draw_text(dst, 108, y1, "Cancel", 2, sel == 1 ? COL_ARROW : COL_TEXT);
}

static void render_rip_usb(uint8_t *dst)
{
    int i, y;

    clear_ui(dst);
    draw_text_centered(dst, 28, "Select USB", 3, COL_TEXT);
    y = 100;
    for (i = 0; i < g_rip.nusb; i++) {
        if (i == g_rip.usb_sel) {
            fill_rect(dst, 40, y - 6, FB_W - 80, 32, COL_BAR);
            fill_rect(dst, 40, y - 6, 8, 32, COL_ACCENT);
            draw_text(dst, 56, y, ">", 2, COL_ARROW);
        }
        draw_text_max(dst, 88, y, g_rip.usb_list[i], 2,
                      i == g_rip.usb_sel ? COL_ARROW : COL_TEXT, 560);
        y += 36;
    }
    draw_text_centered(dst, 420, "Confirm   B: Back", 2, COL_DIM);
}

static void render_rip_run(uint8_t *dst)
{
    int w, fill;
    char pct[16];

    clear_ui(dst);
    draw_text_centered(dst, 28, "Ripping DVD", 3, COL_TEXT);
    draw_text_centered(dst, 80, g_rip.title, 2, COL_ARROW);
    w = FB_W - 160;
    fill_rect(dst, 80, 160, w, 28, COL_BAR);
    fill = (g_rip.pct * w) / 100;
    if (fill < 0)
        fill = 0;
    if (fill > w)
        fill = w;
    if (fill > 0)
        fill_rect(dst, 80, 160, fill, 28, COL_ACCENT);
    snprintf(pct, sizeof(pct), "%d%%", g_rip.pct);
    draw_text_centered(dst, 208, pct, 3, COL_TEXT);
    draw_text_centered(dst, 268, g_rip.msg[0] ? g_rip.msg : "Working...",
                       2, COL_DIM);
    draw_text_centered(dst, 420, "BACK: Cancel", 2, COL_DIM);
}

static void render_rip_cancel(uint8_t *dst)
{
    int y0 = 220, y1 = 264;
    int sel = g_rip.cancel_sel;
    int yb = sel ? y1 : y0;

    clear_ui(dst);
    draw_text_centered(dst, 100, "Cancel rip?", 3, COL_TEXT);
    fill_rect(dst, 80, yb - 8, FB_W - 160, 40, COL_BAR);
    fill_rect(dst, 80, yb - 8, 8, 40, COL_ACCENT);
    draw_text(dst, 108, y0, "No", 2, sel == 0 ? COL_ARROW : COL_TEXT);
    draw_text(dst, 108, y1, "Yes", 2, sel == 1 ? COL_ARROW : COL_TEXT);
    draw_text(dst, 84, yb, ">", 2, COL_ARROW);
}

static void render_rip_done(uint8_t *dst)
{
    int y0 = 260, y1 = 304;
    int sel = g_rip.done_sel;
    int yb = sel ? y1 : y0;

    clear_ui(dst);
    draw_text_centered(dst, 40, "Rip complete", 3, COL_TEXT);
    draw_text_centered(dst, 120, g_rip.done_name[0] ? g_rip.done_name
                                                    : g_rip.file, 2, COL_ARROW);
    fill_rect(dst, 80, yb - 8, FB_W - 160, 40, COL_BAR);
    fill_rect(dst, 80, yb - 8, 8, 40, COL_ACCENT);
    draw_text(dst, 108, y0, "DVD Library", 2, sel == 0 ? COL_ARROW : COL_TEXT);
    draw_text(dst, 108, y1, "Back", 2, sel == 1 ? COL_ARROW : COL_TEXT);
    draw_text(dst, 84, yb, ">", 2, COL_ARROW);
}

static void render_rip_fail(uint8_t *dst)
{
    clear_ui(dst);
    draw_text_centered(dst, 100, "Rip failed", 3, COL_TEXT);
    if (g_rip.fail[0]) {
        int scale = (int)strlen(g_rip.fail) > 36 ? 1 : 2;
        draw_text_centered(dst, 200, g_rip.fail, scale, COL_DIM);
    }
    draw_text_centered(dst, 320, "Press B to return", 2, COL_ARROW);
}

static void rip_poll_status(void)
{
    FILE *f;
    char line[512];
    char stage[32], pct[16], msg[80], path[384], reason[96];

    f = fopen(RIP_STATUS_PATH, "r");
    if (!f)
        return;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }
    fclose(f);
    stage[0] = pct[0] = msg[0] = path[0] = reason[0] = 0;
    rip_kv(line, "stage", stage, sizeof(stage));
    rip_kv(line, "pct", pct, sizeof(pct));
    rip_kv(line, "msg", msg, sizeof(msg));
    rip_kv(line, "path", path, sizeof(path));
    rip_kv(line, "reason", reason, sizeof(reason));
    if (pct[0])
        g_rip.pct = atoi(pct);
    if (msg[0])
        snprintf(g_rip.msg, sizeof(g_rip.msg), "%s", msg);
    if (!strcmp(stage, "complete") && path[0]) {
        const char *slash = strrchr(path, '/');
        snprintf(g_rip.done_name, sizeof(g_rip.done_name), "%s",
                 slash ? slash + 1 : path);
    }
    if (!strcmp(stage, "failed") && reason[0])
        snprintf(g_rip.fail, sizeof(g_rip.fail), "%s", reason);
    if (!strcmp(stage, "cancelled"))
        snprintf(g_rip.fail, sizeof(g_rip.fail), "Cancelled");
}

static int rip_start(void)
{
    pid_t pid;

    unlink(RIP_STATUS_PATH);
    g_rip.pct = 0;
    snprintf(g_rip.msg, sizeof(g_rip.msg), "Creating decrypted backup");
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execl(g_rip.helper, "dvd_rip_iso", "rip", SR0_PATH, g_rip.usb_path,
              "--status-file", RIP_STATUS_PATH, (char *)NULL);
        _exit(127);
    }
    g_rip.pid = pid;
    g_rip.running = 1;
    g_child = pid;
    return 0;
}

static int rip_reap(int block)
{
    int status = 0;
    pid_t w;

    if (!g_rip.running || g_rip.pid <= 0)
        return -1;
    w = waitpid(g_rip.pid, &status, block ? 0 : WNOHANG);
    if (w == 0)
        return -1;
    g_rip.running = 0;
    g_rip.pid = 0;
    g_child = 0;
    rip_poll_status();
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 2;
    return 1;
}

static void rip_request_cancel(void)
{
    if (g_rip.pid > 0)
        kill(g_rip.pid, SIGTERM);
}

static void rip_wait_cancel(void)
{
    int i;

    rip_request_cancel();
    for (i = 0; i < 50; i++) {
        if (rip_reap(0) >= 0)
            return;
        usleep(100000);
    }
    if (g_rip.pid > 0)
        kill(g_rip.pid, SIGKILL);
    rip_reap(1);
}

static void present(FBPair *fb, void (*draw)(uint8_t *, void *), void *arg)
{
    draw(fb->fb_a, arg);
    memcpy(fb->fb_b, fb->fb_a, FB_SIZE);
    __sync_synchronize();
    fb->mbox[0] = 0;
    g_mbox_req = 0;
    wait_vsync(fb);
}

static void present_main_full(FBPair *fb, int sel)
{
    rebuild_ui_cache(sel);
    memset(fb->fb_a, 0, FB_SIZE);
    memcpy(fb->fb_a, g_ui_cache, sizeof(g_ui_cache));
    memcpy(fb->fb_b, fb->fb_a, FB_SIZE);
    g_kun_buf[0].valid = 0;
    g_kun_buf[1].valid = 0;
    __sync_synchronize();
    fb->mbox[0] = 0;
    g_mbox_req = 0;
    g_full_redraws++;
    wait_vsync(fb);
}

struct DrawLib { const DvdLibItem *items; int n, sel; };
struct DrawErr { const char *title; const char *msg; };
struct DrawScan { int unused; };

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

static void draw_rip_prep_cb(uint8_t *d, void *arg)
{
    (void)arg;
    render_rip_prep(d);
}

static void draw_rip_usb_cb(uint8_t *d, void *arg)
{
    (void)arg;
    render_rip_usb(d);
}

static void draw_rip_run_cb(uint8_t *d, void *arg)
{
    (void)arg;
    render_rip_run(d);
}

static void draw_rip_cancel_cb(uint8_t *d, void *arg)
{
    (void)arg;
    render_rip_cancel(d);
}

static void draw_rip_done_cb(uint8_t *d, void *arg)
{
    (void)arg;
    render_rip_done(d);
}

static void draw_rip_fail_cb(uint8_t *d, void *arg)
{
    (void)arg;
    render_rip_fail(d);
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

static int run_player(FBPair *fb, Pad *pad, const char *player, const char *source)
{
    pid_t pid;
    int status = 0;
    uint32_t bits = 0;

    fprintf(stderr, "LAUNCHER: starting %s\n", source);
    fprintf(stderr,
            "LAUNCHER ARGV dvd_av_threaded_test %s "
            "--buffered-yuv-video --fpga-yuv420 --fpga-yuv420-subtitles "
            "--initial-video-skip 1 --video-advance-ms 20 --authored-start\n",
            source);
    fprintf(stderr, "PLAYER START\n");
    set_launcher_mode(MODE_PLAYER_ACTIVE, "start player");
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "LAUNCHER: fork failed: %s\n", strerror(errno));
        set_launcher_mode(MODE_LAUNCHER_ACTIVE, "fork failed");
        return -1;
    }
    if (pid == 0) {
        execl(player, "dvd_av_threaded_test", source,
              "--buffered-yuv-video",
              "--fpga-yuv420",
              "--fpga-yuv420-subtitles",
              "--initial-video-skip", "1",
              "--video-advance-ms", "20",
              "--authored-start",
              (char *)NULL);
        fprintf(stderr, "LAUNCHER: exec failed: %s\n", strerror(errno));
        _exit(127);
    }
    g_child = pid;
    /*
     * Do not run launcher UI/actions while the child plays. Both processes
     * read FPGA joystick at 0x30400008 independently; launcher must not
     * write mailbox A/B or treat Confirm as a launcher selection.
     */
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);

        if (w == pid)
            break;
        if (w < 0) {
            if (errno == EINTR)
                continue;
            g_child = 0;
            fprintf(stderr, "PLAYER EXIT\n");
            wait_controller_neutral(fb);
            if (pad)
                pad->primed = 0;
            set_launcher_mode(MODE_LAUNCHER_ACTIVE, "waitpid error");
            return -1;
        }
        bits = 0;
        if (read_buttons(fb, &bits) == 0)
            log_launcher_input(bits);
        usleep(CTRL_POLL_US);
        if (g_stop)
            break;
    }
    g_child = 0;
    fprintf(stderr, "PLAYER EXIT\n");
    wait_controller_neutral(fb);
    if (pad)
        pad->primed = 0;
    set_launcher_mode(MODE_LAUNCHER_ACTIVE, "player exited");
    if (g_stop)
        return -1;
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
    char rip_helper[512];
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
    if (resolve_rip_helper(rip_helper, sizeof(rip_helper),
                           argc > 0 ? argv[0] : NULL) < 0)
        rip_helper[0] = 0;
    if (map_fb(&fb) < 0)
        return 1;
    if (claim_launcher_singleton() < 0)
        return 1;
    kun_init();

    fprintf(stderr, "MiSTer DVD Player %s\n", DVD_PLAYER_VERSION);
    set_launcher_mode(MODE_LAUNCHER_ACTIVE, "startup");

    while (!g_stop) {
        uint32_t bits = 0;
        int act = ACT_NONE;
        int64_t t = now_us();

        /*
         * A player started outside this process (SSH hardware test) still
         * shares FPGA joystick + mailbox. Become inert until it exits.
         */
        if (g_launcher_mode == MODE_LAUNCHER_ACTIVE && g_child == 0 &&
            player_process_running()) {
            fprintf(stderr, "PLAYER START\n");
            set_launcher_mode(MODE_PLAYER_ACTIVE, "detected player process");
        }
        if (g_launcher_mode == MODE_PLAYER_ACTIVE && g_child == 0) {
            bits = 0;
            if (read_buttons(&fb, &bits) == 0)
                log_launcher_input(bits);
            if (!player_process_running()) {
                fprintf(stderr, "PLAYER EXIT\n");
                wait_controller_neutral(&fb);
                pad.primed = 0;
                set_launcher_mode(MODE_LAUNCHER_ACTIVE, "foreign player exited");
                dirty = 1;
            } else {
                usleep(CTRL_POLL_US);
                continue;
            }
        }

        if (dirty && g_launcher_mode == MODE_LAUNCHER_ACTIVE) {
            if (screen == SCR_MAIN) {
                present_main_full(&fb, main_sel);
                if (g_kun.enabled)
                    mascot_present_dirty(&fb);
            } else if (screen == SCR_LIBRARY) {
                struct DrawLib a = { library, n_lib, lib_sel };
                present(&fb, draw_lib_cb, &a);
            } else if (screen == SCR_RIP_PREP) {
                present(&fb, draw_rip_prep_cb, NULL);
            } else if (screen == SCR_RIP_USB) {
                present(&fb, draw_rip_usb_cb, NULL);
            } else if (screen == SCR_RIP_RUN) {
                present(&fb, draw_rip_run_cb, NULL);
            } else if (screen == SCR_RIP_CANCEL) {
                present(&fb, draw_rip_cancel_cb, NULL);
            } else if (screen == SCR_RIP_DONE) {
                present(&fb, draw_rip_done_cb, NULL);
            } else if (screen == SCR_RIP_FAIL) {
                present(&fb, draw_rip_fail_cb, NULL);
            } else {
                struct DrawErr a = { err_title, err_msg };
                present(&fb, draw_err_cb, &a);
            }
            dirty = 0;
        }

        if (read_buttons(&fb, &bits) == 0) {
            log_launcher_input(bits);
            if (g_launcher_mode == MODE_LAUNCHER_ACTIVE) {
                if (screen == SCR_MAIN && cheat_poll(&pad, bits, t))
                    dirty = 1;
                if (screen != SCR_MAIN)
                    cheat_reset();
                act = pad_poll(&pad, bits, t);
                if (act != ACT_NONE)
                    log_launcher_action(act == ACT_UP ? "UP" :
                                        act == ACT_DOWN ? "DOWN" :
                                        act == ACT_CONFIRM ? "CONFIRM" :
                                        act == ACT_CANCEL ? "CANCEL" : "?");
            }
        }

        if (screen == SCR_MAIN && g_kun.enabled &&
            g_launcher_mode == MODE_LAUNCHER_ACTIVE) {
            if (!g_kun.last_us)
                g_kun.last_us = t;
            if (t - g_kun.last_us >= KUN_PERIOD_US) {
                kun_step();
                g_kun.last_us = t;
                mascot_present_dirty(&fb);
            }
        }

        if ((screen == SCR_RIP_RUN || screen == SCR_RIP_CANCEL) &&
            g_rip.running) {
            int prev = g_rip.pct;
            char prev_msg[80];
            int rc;

            snprintf(prev_msg, sizeof(prev_msg), "%s", g_rip.msg);
            rip_poll_status();
            if (g_rip.pct != prev || strcmp(prev_msg, g_rip.msg) != 0)
                dirty = 1;
            rc = rip_reap(0);
            if (rc >= 0) {
                if (rc == 0) {
                    dvd_library_invalidate_cache();
                    screen = SCR_RIP_DONE;
                    g_rip.done_sel = 0;
                } else if (rc == 2) {
                    screen = SCR_MAIN;
                } else {
                    rip_map_fail(rc);
                    screen = SCR_RIP_FAIL;
                }
                pad.primed = 0;
                dirty = 1;
            }
        }

        if (g_launcher_mode == MODE_LAUNCHER_ACTIVE && screen == SCR_MAIN) {
            if (act == ACT_UP && main_sel > 0) {
                main_sel--;
                dirty = 1;
            } else if (act == ACT_DOWN && main_sel < 2) {
                main_sel++;
                dirty = 1;
            } else if (act == ACT_CONFIRM) {
                if (main_sel == 0) {
                    int rc;
                    fprintf(stderr, "LAUNCHER: Play Physical DVD\n");
                    log_launcher_action("CONFIRM Play Physical DVD");
                    if (!disc_present()) {
                        err_title = "Unable to read DVD";
                        err_msg = "No disc in drive";
                        err_back = SCR_MAIN;
                        screen = SCR_ERROR;
                        pad.primed = 0;
                        dirty = 1;
                    } else {
                        rc = run_player(&fb, &pad, player, SR0_PATH);
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
                } else if (main_sel == 1) {
                    struct DrawScan scan = { 0 };
                    fprintf(stderr, "LAUNCHER: DVD Library\n");
                    present(&fb, draw_scan_cb, &scan);
                    n_lib = dvd_library_refresh(library, DVD_LIB_MAX_ITEMS,
                                                &lib_stats);
                    lib_sel = 0;
                    screen = SCR_LIBRARY;
                    pad.primed = 0;
                    dirty = 1;
                } else {
                    int next;

                    fprintf(stderr, "LAUNCHER: Rip DVD to USB\n");
                    if (!rip_helper[0]) {
                        err_title = "Rip DVD";
                        err_msg = "Rip helper not found";
                        err_back = SCR_MAIN;
                        screen = SCR_ERROR;
                    } else {
                        next = rip_enter(rip_helper);
                        if (next < 0) {
                            err_title = "Rip DVD";
                            err_msg = g_rip.fail[0] ? g_rip.fail
                                                    : "Unable to read DVD";
                            err_back = SCR_MAIN;
                            screen = SCR_ERROR;
                        } else {
                            screen = next;
                        }
                    }
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
                    log_launcher_action("CONFIRM DVD Library play");
                    rc = run_player(&fb, &pad, player, library[lib_sel].path);
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
        } else if (screen == SCR_RIP_USB) {
            if (act == ACT_CANCEL) {
                screen = SCR_MAIN;
                pad.primed = 0;
                dirty = 1;
            } else if (act == ACT_UP && g_rip.usb_sel > 0) {
                g_rip.usb_sel--;
                dirty = 1;
            } else if (act == ACT_DOWN && g_rip.usb_sel < g_rip.nusb - 1) {
                g_rip.usb_sel++;
                dirty = 1;
            } else if (act == ACT_CONFIRM) {
                rip_apply_usb(g_rip.usb_sel);
                g_rip.prep_sel = 0;
                screen = SCR_RIP_PREP;
                pad.primed = 0;
                dirty = 1;
            }
        } else if (screen == SCR_RIP_PREP) {
            if (act == ACT_CANCEL ||
                (act == ACT_CONFIRM && g_rip.prep_sel == 1)) {
                screen = SCR_MAIN;
                pad.primed = 0;
                dirty = 1;
            } else if (act == ACT_UP || act == ACT_DOWN) {
                g_rip.prep_sel = g_rip.prep_sel ? 0 : 1;
                dirty = 1;
            } else if (act == ACT_CONFIRM && g_rip.prep_sel == 0) {
                char why[96];

                if (rip_preflight(why, sizeof(why)) != 0) {
                    snprintf(g_rip.fail, sizeof(g_rip.fail), "%s", why);
                    screen = SCR_RIP_FAIL;
                } else if (rip_start() != 0) {
                    snprintf(g_rip.fail, sizeof(g_rip.fail), "Could not start rip");
                    screen = SCR_RIP_FAIL;
                } else {
                    screen = SCR_RIP_RUN;
                }
                pad.primed = 0;
                dirty = 1;
            }
        } else if (screen == SCR_RIP_RUN) {
            if (act == ACT_CANCEL) {
                g_rip.cancel_sel = 0;
                screen = SCR_RIP_CANCEL;
                pad.primed = 0;
                dirty = 1;
            }
        } else if (screen == SCR_RIP_CANCEL) {
            if (act == ACT_UP || act == ACT_DOWN) {
                g_rip.cancel_sel = g_rip.cancel_sel ? 0 : 1;
                dirty = 1;
            } else if (act == ACT_CANCEL ||
                       (act == ACT_CONFIRM && g_rip.cancel_sel == 0)) {
                screen = SCR_RIP_RUN;
                pad.primed = 0;
                dirty = 1;
            } else if (act == ACT_CONFIRM && g_rip.cancel_sel == 1) {
                snprintf(g_rip.msg, sizeof(g_rip.msg), "Cancelling...");
                present(&fb, draw_rip_run_cb, NULL);
                rip_wait_cancel();
                screen = SCR_MAIN;
                pad.primed = 0;
                dirty = 1;
            }
        } else if (screen == SCR_RIP_DONE) {
            if (act == ACT_UP || act == ACT_DOWN) {
                g_rip.done_sel = g_rip.done_sel ? 0 : 1;
                dirty = 1;
            } else if (act == ACT_CANCEL ||
                       (act == ACT_CONFIRM && g_rip.done_sel == 1)) {
                screen = SCR_MAIN;
                pad.primed = 0;
                dirty = 1;
            } else if (act == ACT_CONFIRM && g_rip.done_sel == 0) {
                struct DrawScan scan = { 0 };

                present(&fb, draw_scan_cb, &scan);
                n_lib = dvd_library_refresh(library, DVD_LIB_MAX_ITEMS,
                                            &lib_stats);
                lib_sel = 0;
                screen = SCR_LIBRARY;
                pad.primed = 0;
                dirty = 1;
            }
        } else if (screen == SCR_RIP_FAIL) {
            if (act == ACT_CANCEL || act == ACT_CONFIRM) {
                screen = SCR_MAIN;
                pad.primed = 0;
                dirty = 1;
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

    if (g_rip.running)
        rip_wait_cancel();

    unmap_fb(&fb);
    clear_launcher_pid();
    if (g_stop)
        fprintf(stderr, "LAUNCHER: stopped by SIGTERM/SIGINT\n");
    fprintf(stderr, "LAUNCHER: exit\n");
    return 0;
}
