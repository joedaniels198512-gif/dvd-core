/*
 * ddr_fb_double_writer.c — fill both MISTER_FB buffers with distinct
 * static patterns so the OSD Buffer A/B option can be verified.
 *
 * Buffer A @ 0x30000000 : colour bars + white border + centre ring
 * Buffer B @ 0x30200000 : contrasting checkerboard
 *
 * Same safety rules as ddr_fb_writer.c: /proc/iomem overlap check,
 * mmap only each framebuffer-sized region, /dev/mem O_RDWR|O_SYNC.
 *
 * Does not flip buffers, pace, or play video. Load DVD.rbf, run this
 * once, then toggle "Buffer" in the core OSD.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define FB_A_PHYS     0x30000000UL
#define FB_B_PHYS     0x30200000UL
#define FB_W          720
#define FB_H          576
#define FB_STRIDE     2880
#define FB_SIZE       ((size_t)FB_STRIDE * FB_H) /* 0x195000 */

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

static int check_range(unsigned long base)
{
    fprintf(stderr, "Target range: 0x%08lx-0x%08lx (%zu bytes)\n",
            base, base + FB_SIZE - 1, FB_SIZE);

    fprintf(stderr, "Checking /proc/iomem for System RAM overlap...\n");

    int ov = overlaps_system_ram(base, FB_SIZE);

    if (ov > 0) {
        fprintf(stderr,
                "FAIL: 0x%08lx overlaps Linux System RAM. Refusing to write.\n",
                base);
        return -1;
    }
    if (ov < 0)
        fprintf(stderr,
                "WARNING: could not verify via /proc/iomem. "
                "Proceeding on the documented MiSTer memory map.\n");
    else
        fprintf(stderr, "OK: no overlap with System RAM.\n");

    return 0;
}

static uint8_t *map_buf(int fd, unsigned long base)
{
    uint8_t *mem = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, (off_t)base);

    if (mem == MAP_FAILED) {
        fprintf(stderr, "mmap 0x%08lx: %s\n", base, strerror(errno));
        return NULL;
    }

    fprintf(stderr, "Mapped %zu bytes at 0x%08lx.\n", FB_SIZE, base);
    return mem;
}

static inline uint32_t px(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Same colour-bar pattern as ddr_fb_writer (Buffer A). */
static void draw_bars(uint8_t *mem)
{
    static const uint8_t bars[8][3] = {
        { 255, 255, 255 },
        { 255, 255,   0 },
        {   0, 255, 255 },
        {   0, 255,   0 },
        { 255,   0, 255 },
        { 255,   0,   0 },
        {   0,   0, 255 },
        {  32,  32,  32 },
    };

    const int border = 4;
    const int cx = FB_W / 2;
    const int cy = FB_H / 2;
    const int radius = FB_H / 4;
    const int ring = 5;
    const int r_out2 = (radius + ring) * (radius + ring);
    const int r_in2  = (radius - ring) * (radius - ring);

    for (int y = 0; y < FB_H; y++) {

        uint32_t *row = (uint32_t *)(mem + (size_t)y * FB_STRIDE);

        for (int x = 0; x < FB_W; x++) {

            int bar = (x * 8) / FB_W;
            uint32_t c = px(bars[bar][0], bars[bar][1], bars[bar][2]);

            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;

            if (d2 <= r_out2 && d2 >= r_in2)
                c = px(255, 255, 255);

            if (x < border || y < border ||
                x >= FB_W - border || y >= FB_H - border)
                c = px(255, 255, 255);

            row[x] = c;
        }
    }
}

/* Contrasting checkerboard (Buffer B): magenta/green tiles, yellow border. */
static void draw_checker(uint8_t *mem)
{
    const int tile = 32;
    const int border = 8;

    for (int y = 0; y < FB_H; y++) {

        uint32_t *row = (uint32_t *)(mem + (size_t)y * FB_STRIDE);

        for (int x = 0; x < FB_W; x++) {

            uint32_t c = (((x / tile) ^ (y / tile)) & 1)
                             ? px(255, 0, 128)
                             : px(0, 180, 40);

            if (x < border || y < border ||
                x >= FB_W - border || y >= FB_H - border)
                c = px(255, 220, 0);

            row[x] = c;
        }
    }
}

int main(void)
{
    fprintf(stderr, "=== SS1 MISTER_FB DOUBLE-BUFFER FILL ===\n");
    fprintf(stderr, "A: colour bars @ 0x%08lx\n", FB_A_PHYS);
    fprintf(stderr, "B: checkerboard @ 0x%08lx\n\n", FB_B_PHYS);

    if (check_range(FB_A_PHYS) < 0 || check_range(FB_B_PHYS) < 0)
        return 1;

    int fd = open("/dev/mem", O_RDWR | O_SYNC);

    if (fd < 0) {
        fprintf(stderr, "open /dev/mem: %s (need root)\n", strerror(errno));
        return 2;
    }

    uint8_t *a = map_buf(fd, FB_A_PHYS);
    uint8_t *b = map_buf(fd, FB_B_PHYS);

    if (!a || !b) {
        if (a) munmap(a, FB_SIZE);
        if (b) munmap(b, FB_SIZE);
        close(fd);
        return 2;
    }

    draw_bars(a);
    draw_checker(b);

    munmap(a, FB_SIZE);
    munmap(b, FB_SIZE);
    close(fd);

    fprintf(stderr,
            "\nBoth buffers written. Open the DVD core OSD and toggle\n"
            "Buffer A / Buffer B. The switch should be a whole-frame flip.\n");
    return 0;
}
