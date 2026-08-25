#ifndef DVD_LIBRARY_H
#define DVD_LIBRARY_H

#include <stdint.h>

#define DVD_LIB_MAX_ITEMS 96
#define DVD_LIB_MAX_PATH  384
#define DVD_LIB_MAX_NAME  40

typedef enum {
    DVD_LIB_SD = 0,
    DVD_LIB_USB = 1
} DvdLibSource;

typedef struct {
    char path[DVD_LIB_MAX_PATH];
    char display[DVD_LIB_MAX_NAME];
    DvdLibSource source;
    uint64_t size;
    int64_t mtime;
} DvdLibItem;

typedef struct {
    int candidates;
    int accepted;
    int rejected;
    int cache_hits;
    int probes;
    int64_t elapsed_us;
} DvdLibStats;

/*
 * Discover DVD-Video ISOs, using the on-disk cache when path/size/mtime match.
 * Fills out[0..n) sorted by display name. Returns n.
 */
int dvd_library_refresh(DvdLibItem *out, int cap, DvdLibStats *stats);

#endif
