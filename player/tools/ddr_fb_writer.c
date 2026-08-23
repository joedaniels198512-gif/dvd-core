/*
 * ddr_fb_writer.c — ARM-side half of the MISTER_FB proof of concept.
 *
 * The DVD core (fpga/DVD.sv) statically exposes a 720x576 BGR0/XRGB8888
 * framebuffer at physical DDR address 0x30000000 (stride 2880) and ASCAL
 * scales it to the display. This program writes a test pattern directly
 * into that memory through /dev/mem, exactly the way Main_MiSTer maps its
 * own framebuffer (open O_RDWR|O_SYNC + mmap, see Main_MiSTer/shmem.cpp).
 *
 * Safety: before mapping, /proc/iomem is checked to ensure the target range
 * does not overlap any "System RAM" region (i.e. memory owned by Linux).
 * On stock MiSTer the upper 512MB (0x20000000+) is reserved for the FPGA
 * framework and never appears as System RAM.
 *
 * Only the framebuffer-sized region is mapped: 0x30000000..0x30194FFF
 * (576 lines * 2880 bytes = 0x195000 bytes, an exact multiple of 4K pages).
 *
 * Run with the DVD core loaded. No restore is needed: the core's
 * framebuffer is always-on by design and this DDR region belongs to it.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FB_PHYS_BASE  0x30000000UL
#define FB_W          720
#define FB_H          576
#define FB_STRIDE     2880                      /* bytes per line          */
#define FB_SIZE       ((size_t)FB_STRIDE * FB_H) /* 0x195000 = 1,658,880   */

#define DISPLAY_SECONDS 10

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

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

/* XRGB8888, red at bit 16 == BGR0 byte order (matches FB_FORMAT 5'b10110). */
static inline uint32_t px(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void draw_pattern(uint8_t *mem)
{
    static const uint8_t bars[8][3] = {
        { 255, 255, 255 }, /* white      */
        { 255, 255,   0 }, /* yellow     */
        {   0, 255, 255 }, /* cyan       */
        {   0, 255,   0 }, /* green      */
        { 255,   0, 255 }, /* magenta    */
        { 255,   0,   0 }, /* red        */
        {   0,   0, 255 }, /* blue       */
        {  32,  32,  32 }, /* near-black */
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

/* Sequential full-frame fill: measures raw uncached DDR write bandwidth. */
static void fill_frame(uint8_t *mem, uint32_t value)
{
    for (int y = 0; y < FB_H; y++) {

        uint32_t *row = (uint32_t *)(mem + (size_t)y * FB_STRIDE);

        for (int x = 0; x < FB_W; x++)
            row[x] = value;
    }
}

int main(void)
{
    fprintf(stderr,
            "=== SS1 MISTER_FB DDR WRITER (720x576 BGR0 @ 0x%08lx) ===\n",
            FB_PHYS_BASE);

    fprintf(stderr,
            "Target range: 0x%08lx-0x%08lx (%zu bytes, stride %d)\n",
            FB_PHYS_BASE,
            FB_PHYS_BASE + FB_SIZE - 1,
            FB_SIZE, FB_STRIDE);

    /* Sanity check against Linux-owned memory. */
    fprintf(stderr, "Checking /proc/iomem for System RAM overlap...\n");

    int ov = overlaps_system_ram(FB_PHYS_BASE, FB_SIZE);

    if (ov > 0) {
        fprintf(stderr,
                "FAIL: 0x%08lx overlaps Linux System RAM. Refusing to write.\n"
                "This SS1's memory map differs from stock MiSTer - do NOT\n"
                "proceed until the reserved region is confirmed.\n",
                FB_PHYS_BASE);
        return 1;
    }
    if (ov < 0)
        fprintf(stderr,
                "WARNING: could not verify via /proc/iomem (unreadable or "
                "hidden).\nProceeding on the documented MiSTer memory map.\n");
    else
        fprintf(stderr, "OK: no overlap with System RAM.\n");

    /* Map exactly the framebuffer region, the same way Main_MiSTer does. */
    int fd = open("/dev/mem", O_RDWR | O_SYNC);

    if (fd < 0) {
        fprintf(stderr, "open /dev/mem: %s (need root)\n", strerror(errno));
        return 2;
    }

    uint8_t *mem = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, (off_t)FB_PHYS_BASE);

    if (mem == MAP_FAILED) {
        fprintf(stderr, "mmap 0x%08lx: %s\n", FB_PHYS_BASE, strerror(errno));
        close(fd);
        return 2;
    }

    fprintf(stderr, "Mapped %zu bytes at 0x%08lx.\n\n", FB_SIZE, FB_PHYS_BASE);

    /* Raw write bandwidth: one timed full-frame fill (black). */
    double t0 = now_sec();
    fill_frame(mem, px(0, 0, 0));
    double fill_s = now_sec() - t0;

    /* Timed test pattern draw. */
    t0 = now_sec();
    draw_pattern(mem);
    double draw_s = now_sec() - t0;

    fprintf(stderr,
            "=== WRITE PERFORMANCE ===\n"
            "Full-frame fill: %7.3f ms  (%6.1f MB/s)\n"
            "Pattern draw:    %7.3f ms  (%6.1f MB/s)\n\n",
            fill_s * 1000.0, (FB_SIZE / (1024.0 * 1024.0)) / fill_s,
            draw_s * 1000.0, (FB_SIZE / (1024.0 * 1024.0)) / draw_s);

    fprintf(stderr,
            "Pattern is now in DDR. The DVD core / ASCAL should be showing\n"
            "colour bars in a 4:3 window. Holding for %d seconds...\n",
            DISPLAY_SECONDS);

    sleep(DISPLAY_SECONDS);

    munmap(mem, FB_SIZE);
    close(fd);

    fprintf(stderr, "\nDone. (Image remains in DDR; the core keeps showing "
                    "it until overwritten.)\n");
    return 0;
}
