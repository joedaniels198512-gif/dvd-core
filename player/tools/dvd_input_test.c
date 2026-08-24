/*
 * dvd_input_test.c — FPGA→ARM logical controller proof.
 *
 * Maps 0x30400008 (same 4 KB page as the framebuffer mailbox) and prints
 * rising edges of the DVD logical pad. Does not open /dev/input, does not
 * touch libdvdnav or the threaded A/V player.
 *
 * Requires the controller-bridge DVD.rbf:
 *   0x30400000 bit0 = ARM→FPGA FB flip (untouched here)
 *   0x30400008      = FPGA→ARM {magic="DVD1", joystick_0}
 *
 * Bits: 0 Right, 1 Left, 2 Down, 3 Up,
 *       4 Select, 5 Back, 6 Play/Pause, 7 DVD Menu,
 *       8 Previous Chapter, 9 Next Chapter.
 * MiSTer OSD/Home is buttons[0] and does not appear here.
 *
 *   dvd_input_test [-v]
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MB_PHYS         0x30400000UL
#define JOY_OFF         8
#define MB_MAP_SIZE     4096
#define JOY_MAGIC       0x44564431u  /* "DVD1" */

static volatile sig_atomic_t running = 1;

static const char *const names[10] = {
    "RIGHT",
    "LEFT",
    "DOWN",
    "UP",
    "SELECT",
    "BACK",
    "PLAY/PAUSE",
    "DVD MENU",
    "PREVIOUS CHAPTER",
    "NEXT CHAPTER"
};

static void on_sig(int sig)
{
    (void)sig;
    running = 0;
}

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
        if (base <= end && (base + size - 1) >= start)
            overlap = 1;
    }
    fclose(fp);
    return checked ? overlap : -1;
}

static int check_range(unsigned long base, size_t size)
{
    int r = overlaps_system_ram(base, size);
    if (r < 0) {
        fprintf(stderr, "Could not verify /proc/iomem for 0x%08lx\n", base);
        return -1;
    }
    if (r) {
        fprintf(stderr, "REFUSING: 0x%08lx overlaps System RAM\n", base);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int verbose = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            verbose = 1;
        else {
            fprintf(stderr, "Usage: dvd_input_test [-v]\n");
            return 1;
        }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "=== SS1 DVD LOGICAL INPUT TEST ===\n");
    fprintf(stderr, "Controller word 0x%08lx  magic 0x%08x (\"DVD1\")\n",
            (unsigned long)(MB_PHYS + JOY_OFF), JOY_MAGIC);
    fprintf(stderr, "Press buttons (OSD/Home is MiSTer OSD, not DVD MENU).\n");
    if (verbose)
        fprintf(stderr, "Verbose: releases printed too.\n");

    if (check_range(MB_PHYS, MB_MAP_SIZE) < 0)
        return 1;

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open /dev/mem: %s (need root)\n", strerror(errno));
        return 2;
    }
    void *map = mmap(NULL, MB_MAP_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, (off_t)MB_PHYS);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        close(fd);
        return 2;
    }

    volatile uint64_t *joy_word =
        (volatile uint64_t *)((uint8_t *)map + JOY_OFF);

    fprintf(stderr, "Waiting for live DVD controller magic...\n");
    while (running) {
        uint64_t w = *joy_word;
        uint32_t magic = (uint32_t)(w >> 32);
        if (magic == JOY_MAGIC)
            break;
        usleep(200000);
    }
    if (!running) {
        munmap(map, MB_MAP_SIZE);
        close(fd);
        return 0;
    }
    fprintf(stderr, "Magic OK. Edge-detecting logical actions.\n");

    uint32_t prev = (uint32_t)(*joy_word);
    while (running) {
        uint64_t w = *joy_word;
        uint32_t magic = (uint32_t)(w >> 32);
        uint32_t now = (uint32_t)w;
        if (magic != JOY_MAGIC) {
            fprintf(stderr, "Magic lost (got 0x%08x) — is DVD.rbf still loaded?\n",
                    magic);
            prev = now;
            usleep(200000);
            continue;
        }
        uint32_t changed = now ^ prev;
        for (i = 0; i < 10; i++) {
            uint32_t bit = 1u << i;
            if (!(changed & bit))
                continue;
            if (now & bit)
                printf("%s\n", names[i]);
            else if (verbose)
                printf("%s released\n", names[i]);
        }
        prev = now;
        usleep(500);
    }

    munmap(map, MB_MAP_SIZE);
    close(fd);
    return 0;
}
