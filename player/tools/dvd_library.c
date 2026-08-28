/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dvd_library.c — DVD-Video ISO discovery, validation, and cache.
 *
 * Scan roots (no unbounded recursion):
 *   /media/fat/DVD/isos              files only
 *   /media/usb*                      files only (root)
 *   /media/usb* /DVD                 files + one subdirectory
 *   /media/usb* /Movies              files + one subdirectory
 *
 * Validation: libdvdread only (no FFmpeg, no playback).
 */

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#define _GNU_SOURCE

#include "dvd_library.h"

#include <ctype.h>
#include <dirent.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/dvd_udf.h>
#include <dvdread/ifo_read.h>
#include <dvdread/ifo_types.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* 32-bit glibc stat() without LFS returns EOVERFLOW for files >2 GiB. */
_Static_assert(sizeof(off_t) >= 8, "need 64-bit off_t for DVD-9 ISOs");
_Static_assert(sizeof(((struct stat *)0)->st_size) >= 8,
               "st_size must be 64-bit");

#define DVD_LIB_SD_DEFAULT      "/media/fat/DVD/isos"
#define DVD_LIB_USB_PARENT_DEF  "/media"
#define DVD_LIB_CACHE_DEFAULT   "/media/fat/DVD/cache/library.cache"

#define MAX_CAND   256
#define MAX_CACHE  256

typedef struct {
    char path[DVD_LIB_MAX_PATH];
    DvdLibSource source;
} Cand;

typedef struct {
    char path[DVD_LIB_MAX_PATH];
    uint64_t size;
    int64_t mtime;
    int valid;
    char display[DVD_LIB_MAX_NAME];
} CacheEnt;

static int g_debug;

static int debug_on(void)
{
    const char *e;

    if (g_debug)
        return 1;
    e = getenv("DVD_LAUNCHER_DEBUG");
    return e && e[0] && e[0] != '0';
}

static const char *sd_dir(void)
{
    const char *e = getenv("DVD_LAUNCHER_SD");
    return (e && e[0]) ? e : DVD_LIB_SD_DEFAULT;
}

static const char *usb_parent(void)
{
    const char *e = getenv("DVD_LAUNCHER_USB_PARENT");
    return (e && e[0]) ? e : DVD_LIB_USB_PARENT_DEF;
}

static const char *cache_path(void)
{
    const char *e = getenv("DVD_LAUNCHER_CACHE");
    return (e && e[0]) ? e : DVD_LIB_CACHE_DEFAULT;
}

static int64_t mono_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int is_iso_name(const char *name)
{
    size_t n;

    if (!name)
        return 0;
    n = strlen(name);
    return n >= 5 && strcasecmp(name + n - 4, ".iso") == 0;
}

static int is_usb_mount_name(const char *name)
{
    const char *p;

    if (!name || strncmp(name, "usb", 3) != 0)
        return 0;
    for (p = name + 3; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return 0;
    }
    return 1;
}

static int lfs_stat(const char *path, struct stat *st)
{
    if (stat(path, st) == 0)
        return 0;
    if (g_debug || debug_on())
        fprintf(stderr, "LIBRARY: stat failed %s: %s\n",
                path, strerror(errno));
    return -1;
}

static int append_cand(Cand *c, int *n, int cap, const char *path,
                       DvdLibSource src)
{
    size_t len;

    if (*n >= cap)
        return 0;
    len = strlen(path);
    if (len == 0 || len >= DVD_LIB_MAX_PATH)
        return 0;
    memcpy(c[*n].path, path, len + 1);
    c[*n].source = src;
    (*n)++;
    return 1;
}

static void collect_dir(const char *dir, int subdirs, DvdLibSource src,
                        Cand *c, int *n, int cap)
{
    DIR *d;
    struct dirent *ent;
    struct stat st;
    char path[DVD_LIB_MAX_PATH];

    d = opendir(dir);
    if (!d)
        return;
    while (*n < cap && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >=
            (int)sizeof(path))
            continue;
        if (lfs_stat(path, &st) != 0)
            continue;
        if (S_ISREG(st.st_mode) && is_iso_name(ent->d_name))
            append_cand(c, n, cap, path, src);
        else if (subdirs && S_ISDIR(st.st_mode))
            collect_dir(path, 0, src, c, n, cap);
    }
    closedir(d);
}

static void collect_all(Cand *c, int *n, int cap)
{
    DIR *md;
    struct dirent *ent;
    struct stat st;
    char usb[DVD_LIB_MAX_PATH];
    char sub[DVD_LIB_MAX_PATH];

    *n = 0;
    collect_dir(sd_dir(), 0, DVD_LIB_SD, c, n, cap);

    md = opendir(usb_parent());
    if (!md)
        return;
    while (*n < cap && (ent = readdir(md)) != NULL) {
        if (!is_usb_mount_name(ent->d_name))
            continue;
        if (snprintf(usb, sizeof(usb), "%s/%s", usb_parent(), ent->d_name) >=
            (int)sizeof(usb))
            continue;
        if (lfs_stat(usb, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        collect_dir(usb, 0, DVD_LIB_USB, c, n, cap);
        if (snprintf(sub, sizeof(sub), "%s/DVD", usb) < (int)sizeof(sub))
            collect_dir(sub, 1, DVD_LIB_USB, c, n, cap);
        if (snprintf(sub, sizeof(sub), "%s/Movies", usb) < (int)sizeof(sub))
            collect_dir(sub, 1, DVD_LIB_USB, c, n, cap);
    }
    closedir(md);
}

static void sanitize_title(const char *in, char *out, size_t cap)
{
    size_t i, o = 0;
    int space = 1;

    if (!in || cap < 2) {
        if (out && cap)
            out[0] = 0;
        return;
    }
    for (i = 0; in[i] && o + 1 < cap; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '_' || ch == '\t')
            ch = ' ';
        if (ch < 32 || ch > 126)
            continue;
        if (ch == ' ') {
            if (space)
                continue;
            space = 1;
        } else {
            space = 0;
        }
        out[o++] = (char)ch;
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = 0;
}

static void filename_stem(const char *path, char *out, size_t cap)
{
    const char *base, *slash;
    char buf[DVD_LIB_MAX_PATH];
    size_t n;

    slash = strrchr(path, '/');
    base = slash ? slash + 1 : path;
    snprintf(buf, sizeof(buf), "%s", base);
    n = strlen(buf);
    if (n >= 4 && strcasecmp(buf + n - 4, ".iso") == 0)
        buf[n - 4] = 0;
    sanitize_title(buf, out, cap);
    if (!out[0])
        snprintf(out, cap, "DVD");
}

static void dvdread_quiet(void *p, dvd_logger_level_t level, const char *fmt,
                          va_list ap)
{
    (void)p;
    (void)level;
    (void)fmt;
    (void)ap;
}

int dvd_library_validate_iso(const char *path, char *name, size_t name_cap)
{
    static const dvd_logger_cb quiet = { .pf_log = dvdread_quiet };
    dvd_reader_t *dvd;
    ifo_handle_t *ifo = NULL;
    char vol[64];
    int ok = 0;

    if (name && name_cap)
        name[0] = 0;

    dvd = DVDOpen2(NULL, &quiet, path);
    if (!dvd)
        return 0;
    ifo = ifoOpenVMGI(dvd);
    if (!ifo || !ifo->vmgi_mat)
        goto done;
    if (memcmp(ifo->vmgi_mat->vmg_identifier, "DVDVIDEO-VMG", 12) != 0)
        goto done;
    if (!ifoRead_TT_SRPT(ifo) || !ifo->tt_srpt ||
        ifo->tt_srpt->nr_of_srpts < 1)
        goto done;
    ok = 1;

    vol[0] = 0;
    if (UDFGetVolumeIdentifier(dvd, vol, sizeof(vol)) > 0)
        sanitize_title(vol, name, name_cap);
    if (name && !name[0]) {
        char prov[33];
        memcpy(prov, ifo->vmgi_mat->provider_identifier, 32);
        prov[32] = 0;
        sanitize_title(prov, name, name_cap);
    }
done:
    if (ifo)
        ifoClose(ifo);
    DVDClose(dvd);
    if (ok && name && !name[0])
        filename_stem(path, name, name_cap);
    return ok;
}

static void unescape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    int esc = 0;

    if (!cap)
        return;
    for (; in && *in && o + 1 < cap; in++) {
        if (!esc && *in == '\\') {
            esc = 1;
            continue;
        }
        if (esc) {
            if (*in == 't')
                out[o++] = '\t';
            else if (*in == 'n')
                out[o++] = '\n';
            else
                out[o++] = *in;
            esc = 0;
        } else {
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

static void fescape(FILE *f, const char *s)
{
    for (; s && *s; s++) {
        if (*s == '\\')
            fputs("\\\\", f);
        else if (*s == '\t')
            fputs("\\t", f);
        else if (*s == '\n')
            fputs("\\n", f);
        else
            fputc(*s, f);
    }
}

static int load_cache(CacheEnt *e, int cap)
{
    FILE *f;
    char line[1024];
    int n = 0;

    f = fopen(cache_path(), "r");
    if (!f)
        return 0;
    while (n < cap && fgets(line, sizeof(line), f)) {
        char *p, *tab[5];
        int nt = 0;
        CacheEnt *c;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        p = line;
        tab[nt++] = p;
        for (; *p && nt < 5; p++) {
            if (*p == '\t') {
                *p = 0;
                tab[nt++] = p + 1;
            }
        }
        if (nt < 5)
            continue;
        p = tab[4] + strlen(tab[4]);
        while (p > tab[4] && (p[-1] == '\n' || p[-1] == '\r'))
            *--p = 0;
        c = &e[n];
        memset(c, 0, sizeof(*c));
        unescape(tab[0], c->path, sizeof(c->path));
        c->size = strtoull(tab[1], NULL, 10);
        c->mtime = (int64_t)strtoll(tab[2], NULL, 10);
        c->valid = atoi(tab[3]) ? 1 : 0;
        unescape(tab[4], c->display, sizeof(c->display));
        if (!c->path[0])
            continue;
        n++;
    }
    fclose(f);
    return n;
}

static void mkdir_parents(const char *file)
{
    char buf[DVD_LIB_MAX_PATH];
    size_t i, n;

    snprintf(buf, sizeof(buf), "%s", file);
    n = strlen(buf);
    for (i = 1; i < n; i++) {
        if (buf[i] == '/') {
            buf[i] = 0;
            mkdir(buf, 0755);
            buf[i] = '/';
        }
    }
}

static void save_cache(const CacheEnt *e, int n)
{
    char tmp[DVD_LIB_MAX_PATH + 8];
    FILE *f;
    int i, fd;

    mkdir_parents(cache_path());
    snprintf(tmp, sizeof(tmp), "%s.tmp", cache_path());
    f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "# dvd-core library.cache v1\n");
    fprintf(f, "# path\\tsize\\tmtime\\tvalid\\tdisplay\n");
    for (i = 0; i < n; i++) {
        fescape(f, e[i].path);
        fprintf(f, "\t%" PRIu64 "\t%" PRId64 "\t%d\t",
                e[i].size, e[i].mtime, e[i].valid);
        fescape(f, e[i].display);
        fputc('\n', f);
    }
    fflush(f);
    fd = fileno(f);
    if (fd >= 0)
        fsync(fd);
    fclose(f);
    if (rename(tmp, cache_path()) != 0)
        unlink(tmp);
}

static CacheEnt *cache_find(CacheEnt *e, int n, const char *path)
{
    int i;

    for (i = 0; i < n; i++) {
        if (strcmp(e[i].path, path) == 0)
            return &e[i];
    }
    return NULL;
}

static int item_cmp(const void *a, const void *b)
{
    const DvdLibItem *x = a, *y = b;
    int c = strcasecmp(x->display, y->display);

    if (c)
        return c;
    return strcmp(x->path, y->path);
}

int dvd_library_refresh(DvdLibItem *out, int cap, DvdLibStats *stats)
{
    Cand cand[MAX_CAND];
    CacheEnt oldc[MAX_CACHE];
    CacheEnt newc[MAX_CACHE];
    DvdLibItem acc[MAX_CAND];
    DvdLibStats st;
    int n_cand = 0, n_old = 0, n_new = 0, n_acc = 0;
    int i, n_out;
    int64_t t0;

    memset(&st, 0, sizeof(st));
    t0 = mono_us();
    g_debug = debug_on();

    collect_all(cand, &n_cand, MAX_CAND);
    n_old = load_cache(oldc, MAX_CACHE);
    st.candidates = n_cand;

    for (i = 0; i < n_cand; i++) {
        struct stat stbuf;
        CacheEnt *hit, *dst;
        char display[DVD_LIB_MAX_NAME];
        int valid;
        uint64_t size;
        int64_t mtime;

        if (lfs_stat(cand[i].path, &stbuf) != 0 || !S_ISREG(stbuf.st_mode))
            continue;
        size = (uint64_t)stbuf.st_size;
        mtime = (int64_t)stbuf.st_mtime;
        display[0] = 0;
        hit = cache_find(oldc, n_old, cand[i].path);
        if (hit && hit->size == size && hit->mtime == mtime) {
            valid = hit->valid;
            snprintf(display, sizeof(display), "%s", hit->display);
            st.cache_hits++;
        } else {
            st.probes++;
            valid = dvd_library_validate_iso(cand[i].path, display, sizeof(display));
            if (!valid) {
                display[0] = 0;
                if (g_debug)
                    fprintf(stderr, "LIBRARY: reject non-DVD %s\n",
                            cand[i].path);
            }
        }
        if (valid)
            st.accepted++;
        else
            st.rejected++;

        if (n_new < MAX_CACHE) {
            dst = &newc[n_new++];
            memset(dst, 0, sizeof(*dst));
            memcpy(dst->path, cand[i].path, DVD_LIB_MAX_PATH);
            dst->size = size;
            dst->mtime = mtime;
            dst->valid = valid;
            memcpy(dst->display, display, DVD_LIB_MAX_NAME);
        }
        if (valid && n_acc < MAX_CAND) {
            DvdLibItem *it = &acc[n_acc++];
            memset(it, 0, sizeof(*it));
            memcpy(it->path, cand[i].path, DVD_LIB_MAX_PATH);
            if (display[0])
                snprintf(it->display, sizeof(it->display), "%s", display);
            else
                filename_stem(cand[i].path, it->display, sizeof(it->display));
            it->source = cand[i].source;
            it->size = size;
            it->mtime = mtime;
        }
    }

    save_cache(newc, n_new);
    if (n_acc > 1)
        qsort(acc, (size_t)n_acc, sizeof(acc[0]), item_cmp);

    n_out = n_acc;
    if (cap < 0)
        cap = 0;
    if (n_out > cap)
        n_out = cap;
    if (out && n_out)
        memcpy(out, acc, (size_t)n_out * sizeof(acc[0]));

    st.elapsed_us = mono_us() - t0;
    fprintf(stderr,
            "LIBRARY: %d candidates, %d DVDs, %d rejected, %d cache hits, "
            "%d probes, %lldms\n",
            st.candidates, st.accepted, st.rejected, st.cache_hits, st.probes,
            (long long)((st.elapsed_us + 500) / 1000));
    if (stats)
        *stats = st;
    return n_out;
}

void dvd_library_invalidate_cache(void)
{
    unlink(cache_path());
}

#ifdef DVD_LIBRARY_STANDALONE
int main(void)
{
    DvdLibItem items[DVD_LIB_MAX_ITEMS];
    DvdLibStats st;
    int n, i;

    n = dvd_library_refresh(items, DVD_LIB_MAX_ITEMS, &st);
    for (i = 0; i < n; i++)
        printf("%s\t%s\t%s\n",
               items[i].source == DVD_LIB_SD ? "SD" : "USB",
               items[i].display, items[i].path);
    return 0;
}
#endif
