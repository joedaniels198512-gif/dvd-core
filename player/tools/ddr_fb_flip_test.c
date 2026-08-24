/*
 * ddr_fb_flip_test.c — ARM-controlled MISTER_FB double-buffer flip test.
 *
 * Renders only into the inactive buffer, then writes mailbox bit 0 at
 * 0x30400000 (0 = A @ 0x30000000, 1 = B @ 0x30200000). The DVD core
 * reads that word once per FB_VBL and publishes FB_BASE.
 *
 * Not the DVD player. ~300 flips. Leave OSD Buffer on A so mailbox
 * control is the only requester (OSD B is ORed in the FPGA).
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
#include <unistd.h>

#define FB_A_PHYS     0x30000000UL
#define FB_B_PHYS     0x30200000UL
#define MB_PHYS       0x30400000UL
#define FB_W          720
#define FB_H          576
#define FB_STRIDE     2880
#define FB_SIZE       ((size_t)FB_STRIDE * FB_H)
#define MB_MAP_SIZE   4096
#define FLIPS         300

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

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

        if (base <= end && (base + size - 1) >= start)
            overlap = 1;
    }

    fclose(fp);
    return checked ? overlap : -1;
}

static int check_range(unsigned long base, size_t size)
{
    fprintf(stderr, "  0x%08lx-0x%08lx\n", base, base + size - 1);

    int ov = overlaps_system_ram(base, size);

    if (ov > 0) {
        fprintf(stderr, "FAIL: overlaps System RAM\n");
        return -1;
    }
    if (ov < 0)
        fprintf(stderr, "WARNING: /proc/iomem not verifiable\n");

    return 0;
}

static inline uint32_t px(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/*
 * Distinctive full-frame pattern. Even flips: red field + white moving
 * bar. Odd flips: blue field + yellow moving bar. A torn frame would
 * show both colours in one picture.
 */
static void draw_flip_pattern(uint8_t *mem, int flip)
{
    const int even = ((flip & 1) == 0);
    const uint32_t field = even ? px(200, 0, 0) : px(0, 40, 200);
    const uint32_t bar   = even ? px(255, 255, 255) : px(255, 220, 0);
    const uint32_t edge  = even ? px(255, 180, 0) : px(0, 255, 180);

    const int band_h = 48;
    const int band_y = (flip * 16) % (FB_H - band_h);

    for (int y = 0; y < FB_H; y++) {

        uint32_t *row = (uint32_t *)(mem + (size_t)y * FB_STRIDE);
        uint32_t c = field;

        if (y >= band_y && y < band_y + band_h)
            c = bar;

        if (y < 6 || y >= FB_H - 6)
            c = edge;

        for (int x = 0; x < FB_W; x++) {
            if (x < 6 || x >= FB_W - 6)
                row[x] = edge;
            else
                row[x] = c;
        }
    }

    /* Flip count as a thermometer along the top inside the border. */
    {
        uint32_t *row = (uint32_t *)(mem + (size_t)12 * FB_STRIDE);
        int marks = 1 + (flip % 80);

        for (int i = 0; i < marks; i++) {
            int x = 16 + i * 8;
            if (x + 4 < FB_W)
                row[x] = row[x + 1] = row[x + 2] = px(255, 255, 255);
        }
    }
}

static int wait_vsync(int fb_fd)
{
    int arg = 0;

    if (ioctl(fb_fd, FBIO_WAITFORVSYNC, &arg) == 0)
        return 0;

    usleep(20000);
    return -1;
}

int main(void)
{
    fprintf(stderr, "=== SS1 MAILBOX DOUBLE-BUFFER FLIP TEST ===\n");
    fprintf(stderr, "A=0x%08lx  B=0x%08lx  mailbox=0x%08lx  flips=%d\n",
            FB_A_PHYS, FB_B_PHYS, MB_PHYS, FLIPS);
    fprintf(stderr, "Leave OSD Buffer on A.\n");

    fprintf(stderr, "Checking /proc/iomem...\n");
    if (check_range(FB_A_PHYS, FB_SIZE) < 0 ||
        check_range(FB_B_PHYS, FB_SIZE) < 0 ||
        check_range(MB_PHYS, MB_MAP_SIZE) < 0)
        return 1;

    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "open /dev/mem: %s\n", strerror(errno));
        return 2;
    }

    uint8_t *buf_a = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED, mem_fd, (off_t)FB_A_PHYS);
    uint8_t *buf_b = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED, mem_fd, (off_t)FB_B_PHYS);
    void *mb_map = mmap(NULL, MB_MAP_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, mem_fd, (off_t)MB_PHYS);

    if (buf_a == MAP_FAILED || buf_b == MAP_FAILED || mb_map == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 2;
    }

    volatile uint32_t *mbox = mb_map;

    int fb_fd = open("/dev/fb0", O_RDWR);
    int vsync_ok = 1;

    if (fb_fd < 0) {
        fprintf(stderr, "open /dev/fb0: %s (will sleep 20 ms)\n",
                strerror(errno));
        vsync_ok = 0;
    } else if (wait_vsync(fb_fd) < 0) {
        fprintf(stderr, "FBIO_WAITFORVSYNC failed; using 20 ms sleep\n");
        vsync_ok = 0;
    } else {
        fprintf(stderr, "Using FBIO_WAITFORVSYNC for flip cadence.\n");
    }

    /* Start from a known mailbox = A so the first write goes to B. */
    mbox[0] = 0;
    if (fb_fd >= 0) {
        wait_vsync(fb_fd);
        wait_vsync(fb_fd);
    } else {
        usleep(40000);
    }

    int displayed = 0; /* 0 = A on screen */

    for (int n = 0; n < FLIPS; n++) {

        int next = displayed ^ 1;
        uint8_t *dst = next ? buf_b : buf_a;

        draw_flip_pattern(dst, n);
        /* O_SYNC does not order these non-volatile FB stores vs the
         * mailbox write. Full barrier so the FPGA cannot observe the
         * flip request until the inactive buffer is globally visible. */
        __sync_synchronize();
        mbox[0] = (uint32_t)next;

        /*
         * Two HDMI vsyncs: VBL N fetches the mailbox; VBL N+1 publishes
         * it to FB_BASE at the same edge ASCAL samples. One wait would
         * let this process paint a buffer that is still on screen.
         */
        if (fb_fd >= 0) {
            wait_vsync(fb_fd);
            wait_vsync(fb_fd);
        } else {
            usleep(40000);
        }

        displayed = next;

        if ((n + 1) % 50 == 0)
            fprintf(stderr, "Flips %d/%d  now showing %c\n",
                    n + 1, FLIPS, displayed ? 'B' : 'A');
    }

    fprintf(stderr,
            "\nDone. %d flips. %s\n"
            "A torn/partial frame would mix red and blue in one picture.\n",
            FLIPS, vsync_ok ? "Paced by HDMI vsync." : "Paced by 20 ms sleep.");

    munmap(buf_a, FB_SIZE);
    munmap(buf_b, FB_SIZE);
    munmap(mb_map, MB_MAP_SIZE);
    close(mem_fd);
    if (fb_fd >= 0)
        close(fb_fd);

    return 0;
}
