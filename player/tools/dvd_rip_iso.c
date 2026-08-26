/*
 * dvd_rip_iso.c — decrypt a physical DVD to a DVD-Video ISO on USB.
 *
 * Two-stage v1 path (no raw CSS sector copier):
 *   dvdbackup -M  ->  temporary VIDEO_TS tree
 *   genisoimage -dvd-video  ->  <title>.iso.part
 *   validate (same as dvd_library)  ->  rename to .iso
 *
 * Machine-readable progress: /tmp/dvd_rip_status (override --status-file).
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
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MSDOS_SUPER_MAGIC
#define MSDOS_SUPER_MAGIC 0x4d44
#endif

#define SR0_DEFAULT     "/dev/sr0"
#define USB_PARENT      "/media"
#define STATUS_DEFAULT  "/tmp/dvd_rip_status"
#define LOG_DIR         "/media/fat/DVD/logs"
#define LOG_PATH        LOG_DIR "/rip.log"
#define LOG_PREV        LOG_DIR "/rip.previous.log"
#define STAGING_DIR     ".rip_tmp"
#define SAFETY_BYTES    (512ULL * 1024ULL * 1024ULL)
#define FAT32_MAX       (4ULL * 1024ULL * 1024ULL * 1024ULL - 1ULL)
#define NAME_MAX_OUT    80
#define VOL_MAX         32
#define PATH_MAX_RIP    768

enum {
    EX_OK = 0,
    EX_FAIL = 1,
    EX_CANCEL = 2,
    EX_EXISTS = 3,
    EX_FAT32 = 4,
    EX_NOSPACE = 5,
    EX_NODISC = 6,
    EX_READ = 7,
    EX_VALIDATE = 8,
    EX_NOUSB = 9,
    EX_NOTDVD = 10
};

typedef struct {
    char path[PATH_MAX_RIP];
    char fstype[32];
    uint64_t free_bytes;
    int writable;
} UsbDest;

typedef struct {
    char title[64];
    char file_stem[NAME_MAX_OUT];
    char volume[VOL_MAX + 1];
    uint64_t payload;
    int valid;
} DiscInfo;

static volatile sig_atomic_t g_cancel = 0;
static pid_t g_child = 0;
static FILE *g_log = NULL;
static char g_status_path[PATH_MAX_RIP];
static char g_staging[PATH_MAX_RIP];
static char g_part_path[PATH_MAX_RIP];
static char g_iso_path[PATH_MAX_RIP];
static char g_selfdir[PATH_MAX_RIP];

static void on_sig(int sig)
{
    (void)sig;
    g_cancel = 1;
    if (g_child > 0)
        kill(-g_child, SIGTERM);
}

static int64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void log_line(const char *fmt, ...)
{
    va_list ap;
    time_t t;
    struct tm tm;
    char ts[32];

    t = time(NULL);
    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    if (g_log) {
        fprintf(g_log, "%s  ", ts);
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    fprintf(stderr, "RIP: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void status_write(const char *fmt, ...)
{
    char tmp[PATH_MAX_RIP + 8];
    FILE *f;
    va_list ap;

    snprintf(tmp, sizeof(tmp), "%s.tmp", g_status_path);
    f = fopen(tmp, "w");
    if (!f)
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, g_status_path);
}

static void mkdir_p(const char *path)
{
    char buf[PATH_MAX_RIP];
    size_t i, n;

    snprintf(buf, sizeof(buf), "%s", path);
    n = strlen(buf);
    for (i = 1; i < n; i++) {
        if (buf[i] == '/') {
            buf[i] = 0;
            mkdir(buf, 0755);
            buf[i] = '/';
        }
    }
    mkdir(buf, 0755);
}

static uint64_t dir_bytes(const char *root)
{
    DIR *d;
    struct dirent *ent;
    struct stat st;
    char path[PATH_MAX_RIP];
    uint64_t n = 0;

    d = opendir(root);
    if (!d)
        return 0;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", root, ent->d_name) >=
            (int)sizeof(path))
            continue;
        if (lstat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            n += dir_bytes(path);
        else if (S_ISREG(st.st_mode))
            n += (uint64_t)st.st_size;
    }
    closedir(d);
    return n;
}

static int rm_rf(const char *path)
{
    DIR *d;
    struct dirent *ent;
    struct stat st;
    char child[PATH_MAX_RIP];

    if (lstat(path, &st) != 0)
        return (errno == ENOENT) ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        d = opendir(path);
        if (d) {
            while ((ent = readdir(d)) != NULL) {
                if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
                    continue;
                if (snprintf(child, sizeof(child), "%s/%s", path,
                             ent->d_name) >= (int)sizeof(child))
                    continue;
                rm_rf(child);
            }
            closedir(d);
        }
        return rmdir(path);
    }
    return unlink(path);
}

static void cleanup_partial(void)
{
    if (g_part_path[0])
        unlink(g_part_path);
    if (g_staging[0])
        rm_rf(g_staging);
}

static int is_usb_name(const char *name)
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

static void fstype_for(const char *path, char *out, size_t cap)
{
    FILE *f;
    char line[512], spec[256], mp[256], type[64];
    size_t best = 0;

    out[0] = 0;
    f = fopen("/proc/mounts", "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%255s %255s %63s", spec, mp, type) != 3)
            continue;
        {
            size_t n = strlen(mp);
            if (n && n >= best &&
                strncmp(path, mp, n) == 0 &&
                (path[n] == 0 || path[n] == '/' || n == 1)) {
                best = n;
                snprintf(out, cap, "%s", type);
            }
        }
    }
    fclose(f);
}

static int dest_info(const char *path, uint64_t *free_out, char *fstype,
                     size_t ftcap)
{
    struct statvfs vs;
    struct statfs fs;

    if (statvfs(path, &vs) != 0)
        return -1;
    *free_out = (uint64_t)vs.f_bavail * (uint64_t)vs.f_frsize;
    fstype_for(path, fstype, ftcap);
    if (!fstype[0] && statfs(path, &fs) == 0) {
        if ((unsigned long)fs.f_type == MSDOS_SUPER_MAGIC)
            snprintf(fstype, ftcap, "vfat");
    }
    return 0;
}

static int is_fat32(const char *fstype)
{
    return fstype && (!strcasecmp(fstype, "vfat") ||
                      !strcasecmp(fstype, "msdos") ||
                      !strcasecmp(fstype, "fat") ||
                      !strcasecmp(fstype, "fat32"));
}

static int usb_writable(const char *path)
{
    char probe[PATH_MAX_RIP];
    int fd;

    if (access(path, W_OK) != 0)
        return 0;
    snprintf(probe, sizeof(probe), "%s/.dvd_rip_w", path);
    fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 0;
    close(fd);
    unlink(probe);
    return 1;
}

static int list_usb(UsbDest *out, int cap)
{
    DIR *d;
    struct dirent *ent;
    int n = 0;
    char path[PATH_MAX_RIP];
    struct stat st;

    d = opendir(USB_PARENT);
    if (!d)
        return 0;
    while (n < cap && (ent = readdir(d)) != NULL) {
        if (!is_usb_name(ent->d_name))
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", USB_PARENT, ent->d_name) >=
            (int)sizeof(path))
            continue;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].path, sizeof(out[n].path), "%s", path);
        out[n].writable = usb_writable(path);
        dest_info(path, &out[n].free_bytes, out[n].fstype,
                  sizeof(out[n].fstype));
        n++;
    }
    closedir(d);
    return n;
}

static void sanitize_filename(const char *in, char *out, size_t cap)
{
    size_t i, o = 0;
    int space = 1;

    if (!out || cap < 2)
        return;
    out[0] = 0;
    if (!in)
        return;
    for (i = 0; in[i] && o + 1 < cap && o + 1 < NAME_MAX_OUT; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == ':' || ch == '/' || ch == '\\' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
            ch = ' ';
        if (ch < 32 || ch == 127)
            continue;
        if (ch == ' ' || ch == '\t' || ch == '_') {
            if (space)
                continue;
            space = 1;
            out[o++] = ' ';
            continue;
        }
        space = 0;
        out[o++] = (char)ch;
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = 0;
    if (out[0] == '.')
        memmove(out, out + 1, strlen(out));
    if (!out[0])
        snprintf(out, cap, "DVD");
}

static void make_volume(const char *in, char *out, size_t cap)
{
    size_t i, o = 0;
    int us = 1;

    if (!out || cap < 2)
        return;
    out[0] = 0;
    for (i = 0; in && in[i] && o + 1 < cap && o < VOL_MAX; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (islower(ch))
            ch = (unsigned char)toupper(ch);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            out[o++] = (char)ch;
            us = 0;
        } else if (!us && o + 1 < cap) {
            out[o++] = '_';
            us = 1;
        }
    }
    while (o > 0 && out[o - 1] == '_')
        o--;
    out[o] = 0;
    if (!out[0])
        snprintf(out, cap, "DVDVIDEO");
}

static uint64_t estimate_payload(dvd_reader_t *dvd, int nsets)
{
    static const dvd_read_domain_t doms[] = {
        DVD_READ_INFO_FILE,
        DVD_READ_INFO_BACKUP_FILE,
        DVD_READ_MENU_VOBS,
        DVD_READ_TITLE_VOBS
    };
    uint64_t sum = 0;
    int t, d;
    dvd_stat_t st;

    for (t = 0; t <= nsets; t++) {
        for (d = 0; d < 4; d++) {
            memset(&st, 0, sizeof(st));
            if (DVDFileStat(dvd, t, doms[d], &st) == 0 && st.size > 0)
                sum += (uint64_t)st.size;
        }
    }
    return sum;
}

static void dvdread_quiet(void *p, dvd_logger_level_t level, const char *fmt,
                          va_list ap)
{
    (void)p;
    (void)level;
    (void)fmt;
    (void)ap;
}

static int probe_disc(const char *dev, DiscInfo *info)
{
    static const dvd_logger_cb quiet = { .pf_log = dvdread_quiet };
    dvd_reader_t *dvd;
    ifo_handle_t *ifo = NULL;
    char vol[64];
    int nsets = 0;

    memset(info, 0, sizeof(*info));
    if (access(dev, F_OK) != 0)
        return EX_NODISC;
    dvd = DVDOpen2(NULL, &quiet, dev);
    if (!dvd)
        return EX_NOTDVD;
    ifo = ifoOpenVMGI(dvd);
    if (!ifo || !ifo->vmgi_mat ||
        memcmp(ifo->vmgi_mat->vmg_identifier, "DVDVIDEO-VMG", 12) != 0) {
        if (ifo)
            ifoClose(ifo);
        DVDClose(dvd);
        return EX_NOTDVD;
    }
    if (!ifoRead_TT_SRPT(ifo) || !ifo->tt_srpt ||
        ifo->tt_srpt->nr_of_srpts < 1) {
        ifoClose(ifo);
        DVDClose(dvd);
        return EX_NOTDVD;
    }
    nsets = ifo->vmgi_mat->vmg_nr_of_title_sets;
    vol[0] = 0;
    if (UDFGetVolumeIdentifier(dvd, vol, sizeof(vol)) > 0)
        snprintf(info->title, sizeof(info->title), "%s", vol);
    if (!info->title[0]) {
        char prov[33];
        memcpy(prov, ifo->vmgi_mat->provider_identifier, 32);
        prov[32] = 0;
        snprintf(info->title, sizeof(info->title), "%s", prov);
    }
    sanitize_filename(info->title, info->file_stem, sizeof(info->file_stem));
    make_volume(info->file_stem, info->volume, sizeof(info->volume));
    info->payload = estimate_payload(dvd, nsets);
    if (info->payload < 1024ULL * 1024ULL)
        info->payload = (uint64_t)ifo->vmgi_mat->vmg_last_sector * 2048ULL;
    info->valid = 1;
    ifoClose(ifo);
    DVDClose(dvd);
    return EX_OK;
}

static void format_bytes(uint64_t n, char *out, size_t cap)
{
    double gb = n / (1024.0 * 1024.0 * 1024.0);

    if (gb >= 10.0)
        snprintf(out, cap, "%.0f GB", gb);
    else
        snprintf(out, cap, "%.1f GB", gb);
}

static void resolve_selfdir(const char *argv0)
{
    char dir[PATH_MAX_RIP];
    const char *slash;
    size_t n;
    char exe[PATH_MAX_RIP];
    ssize_t k;

    k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k > 0) {
        exe[k] = 0;
        slash = strrchr(exe, '/');
        if (slash) {
            n = (size_t)(slash - exe);
            memcpy(g_selfdir, exe, n);
            g_selfdir[n] = 0;
            return;
        }
    }
    slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (slash) {
        n = (size_t)(slash - argv0);
        if (n >= sizeof(dir))
            n = sizeof(dir) - 1;
        memcpy(g_selfdir, argv0, n);
        g_selfdir[n] = 0;
    } else {
        strcpy(g_selfdir, ".");
    }
}

static int find_tool(const char *name, char *out, size_t cap)
{
    const char *env;
    char cand[PATH_MAX_RIP];

    snprintf(cand, sizeof(cand), "%s/%s", g_selfdir, name);
    if (access(cand, X_OK) == 0) {
        snprintf(out, cap, "%s", cand);
        return 0;
    }
    env = getenv("DVD_ROOT");
    if (env && env[0]) {
        snprintf(cand, sizeof(cand), "%s/dev/%s", env, name);
        if (access(cand, X_OK) == 0) {
            snprintf(out, cap, "%s", cand);
            return 0;
        }
        snprintf(cand, sizeof(cand), "%s/bin/%s", env, name);
        if (access(cand, X_OK) == 0) {
            snprintf(out, cap, "%s", cand);
            return 0;
        }
    }
    snprintf(cand, sizeof(cand), "/media/fat/DVD/dev/%s", name);
    if (access(cand, X_OK) == 0) {
        snprintf(out, cap, "%s", cand);
        return 0;
    }
    snprintf(cand, sizeof(cand), "/media/fat/DVD/bin/%s", name);
    if (access(cand, X_OK) == 0) {
        snprintf(out, cap, "%s", cand);
        return 0;
    }
    return -1;
}

static void setup_child_env(void)
{
    char lib[PATH_MAX_RIP * 3];
    char magic[PATH_MAX_RIP];

    snprintf(lib, sizeof(lib),
             "%s:%s/rip-lib:/media/fat/DVD/lib:/media/fat/DVD/dev/rip-lib",
             g_selfdir, g_selfdir);
    setenv("LD_LIBRARY_PATH", lib, 1);
    snprintf(magic, sizeof(magic), "%s/magic.mgc", g_selfdir);
    if (access(magic, R_OK) == 0)
        setenv("MAGIC", magic, 1);
    else {
        snprintf(magic, sizeof(magic), "%s/rip-lib/magic.mgc", g_selfdir);
        if (access(magic, R_OK) == 0)
            setenv("MAGIC", magic, 1);
    }
}

static int wait_child_progress(pid_t pid, const char *stage, const char *msg,
                               const char *watch, uint64_t total, int pct0,
                               int pct1)
{
    int status = 0;
    int64_t last = 0;

    g_child = pid;
    for (;;) {
        pid_t w;
        uint64_t have = 0;
        int pct;

        if (g_cancel) {
            kill(-pid, SIGTERM);
            usleep(2000000);
            if (waitpid(pid, &status, WNOHANG) == 0)
                kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            g_child = 0;
            return EX_CANCEL;
        }
        w = waitpid(pid, &status, WNOHANG);
        if (watch && watch[0]) {
            struct stat st;
            if (stat(watch, &st) == 0 && S_ISREG(st.st_mode))
                have = (uint64_t)st.st_size;
            else
                have = dir_bytes(watch);
        }
        if (total > 0) {
            double f = (double)have / (double)total;
            if (f > 1.0)
                f = 1.0;
            pct = pct0 + (int)((pct1 - pct0) * f);
        } else {
            pct = pct0;
        }
        if (now_us() - last > 400000) {
            status_write("RIP|stage=%s|pct=%d|bytes=%" PRIu64 "|total=%" PRIu64
                         "|msg=%s",
                         stage, pct, have, total, msg);
            last = now_us();
        }
        if (w == pid)
            break;
        if (w < 0 && errno != EINTR) {
            g_child = 0;
            return EX_FAIL;
        }
        usleep(200000);
    }
    g_child = 0;
    if (WIFSIGNALED(status)) {
        if (g_cancel)
            return EX_CANCEL;
        log_line("%s killed by signal %d", stage, WTERMSIG(status));
        return EX_FAIL;
    }
    if (!WIFEXITED(status))
        return EX_FAIL;
    return WEXITSTATUS(status);
}

static int run_tool(char *const argv[], const char *stage, const char *msg,
                    const char *watch, uint64_t total, int pct0, int pct1)
{
    pid_t pid;
    int rc;

    pid = fork();
    if (pid < 0) {
        log_line("fork failed: %s", strerror(errno));
        return EX_FAIL;
    }
    if (pid == 0) {
        int fd;
        setup_child_env();
        setpgid(0, 0);
        fd = open("/media/fat/DVD/logs/rip.child.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    setpgid(pid, pid);
    rc = wait_child_progress(pid, stage, msg, watch, total, pct0, pct1);
    return rc;
}

static int find_videots_root(const char *staging, char *out, size_t cap)
{
    DIR *d;
    struct dirent *ent;
    char path[PATH_MAX_RIP];
    struct stat st;

    snprintf(path, sizeof(path), "%s/VIDEO_TS", staging);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(out, cap, "%s", staging);
        return 0;
    }
    d = opendir(staging);
    if (!d)
        return -1;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "%s/%s/VIDEO_TS", staging,
                     ent->d_name) >= (int)sizeof(path))
            continue;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(out, cap, "%s/%s", staging, ent->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

static int open_log(void)
{
    mkdir_p(LOG_DIR);
    if (access(LOG_PATH, F_OK) == 0) {
        unlink(LOG_PREV);
        rename(LOG_PATH, LOG_PREV);
    }
    g_log = fopen(LOG_PATH, "w");
    return g_log ? 0 : -1;
}

static int cmd_list_usb(void)
{
    UsbDest u[8];
    int n, i;
    char freeb[32];

    n = list_usb(u, 8);
    for (i = 0; i < n; i++) {
        format_bytes(u[i].free_bytes, freeb, sizeof(freeb));
        printf("RIP|usb=%d|path=%s|free=%" PRIu64 "|fstype=%s|writable=%d\n",
               i, u[i].path, u[i].free_bytes, u[i].fstype[0] ? u[i].fstype : "?",
               u[i].writable);
    }
    if (n == 0)
        printf("RIP|usb=none\n");
    return n ? EX_OK : EX_NOUSB;
}

static int cmd_probe(const char *dev)
{
    DiscInfo info;
    int rc;
    char sz[32];

    rc = probe_disc(dev, &info);
    if (rc != EX_OK) {
        printf("RIP|probe=fail|code=%d\n", rc);
        return rc;
    }
    format_bytes(info.payload, sz, sizeof(sz));
    printf("RIP|probe=ok|title=%s|file=%s.iso|vol=%s|bytes=%" PRIu64
           "|size=%s|css=libdvdcss\n",
           info.file_stem, info.file_stem, info.volume, info.payload, sz);
    return EX_OK;
}

static int validate_tree_files(const char *dvdroot, uint64_t *sum_out)
{
    char vt[PATH_MAX_RIP];
    DIR *d;
    struct dirent *ent;
    struct stat st;
    uint64_t sum = 0;
    int n = 0;

    snprintf(vt, sizeof(vt), "%s/VIDEO_TS", dvdroot);
    d = opendir(vt);
    if (!d)
        return -1;
    while ((ent = readdir(d)) != NULL) {
        char path[PATH_MAX_RIP];
        if (ent->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", vt, ent->d_name) >=
            (int)sizeof(path))
            continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (st.st_size <= 0)
            continue;
        sum += (uint64_t)st.st_size;
        n++;
    }
    closedir(d);
    if (sum_out)
        *sum_out = sum;
    return n;
}

static int cmd_rip(const char *dev, const char *usb_root)
{
    DiscInfo info;
    char dvd_dir[PATH_MAX_RIP];
    char backup_bin[PATH_MAX_RIP];
    char iso_bin[PATH_MAX_RIP];
    char dvdroot[PATH_MAX_RIP];
    char fstype[32];
    char sz[32], fr[32], rq[32];
    uint64_t freeb = 0, required, tree_bytes = 0, iso_size;
    int rc, nfiles;
    struct stat st;

    memset(&info, 0, sizeof(info));
    g_staging[0] = g_part_path[0] = g_iso_path[0] = 0;

    open_log();
    log_line("source=%s dest_usb=%s", dev, usb_root);

    rc = probe_disc(dev, &info);
    if (rc != EX_OK) {
        log_line("probe failed code=%d", rc);
        status_write("RIP|stage=failed|reason=%s",
                     rc == EX_NODISC ? "No disc in drive" :
                     rc == EX_NOTDVD ? "Not a DVD-Video disc" :
                     "Unable to read DVD");
        return rc;
    }
    log_line("title='%s' file='%s.iso' vol='%s' payload=%" PRIu64,
             info.file_stem, info.file_stem, info.volume, info.payload);

    if (access(usb_root, F_OK) != 0) {
        status_write("RIP|stage=failed|reason=No USB drive found");
        return EX_NOUSB;
    }
    snprintf(dvd_dir, sizeof(dvd_dir), "%s/DVD", usb_root);
    mkdir_p(dvd_dir);
    if (!usb_writable(dvd_dir) && !usb_writable(usb_root)) {
        status_write("RIP|stage=failed|reason=USB not writable");
        return EX_NOUSB;
    }
    dest_info(usb_root, &freeb, fstype, sizeof(fstype));
    required = info.payload * 2ULL + SAFETY_BYTES;
    format_bytes(info.payload, sz, sizeof(sz));
    format_bytes(freeb, fr, sizeof(fr));
    format_bytes(required, rq, sizeof(rq));
    log_line("disc=%s usb_free=%s required=%s fstype=%s", sz, fr, rq, fstype);

    if (is_fat32(fstype) && info.payload > FAT32_MAX) {
        log_line("reject: FAT32 too small for this ISO");
        status_write("RIP|stage=failed|reason=This DVD is too large for FAT32. Use an exFAT USB drive.");
        return EX_FAT32;
    }
    if (freeb < required) {
        log_line("reject: not enough free space");
        status_write("RIP|stage=failed|reason=Not enough free space to create ISO.");
        return EX_NOSPACE;
    }

    snprintf(g_iso_path, sizeof(g_iso_path), "%s/%s.iso", dvd_dir,
             info.file_stem);
    snprintf(g_part_path, sizeof(g_part_path), "%s/%s.iso.part", dvd_dir,
             info.file_stem);
    if (stat(g_iso_path, &st) == 0) {
        log_line("ISO already exists: %s", g_iso_path);
        status_write("RIP|stage=failed|reason=ISO already exists");
        return EX_EXISTS;
    }

    if (find_tool("dvdbackup", backup_bin, sizeof(backup_bin)) < 0) {
        log_line("dvdbackup not found");
        status_write("RIP|stage=failed|reason=dvdbackup not found");
        return EX_FAIL;
    }
    if (find_tool("genisoimage", iso_bin, sizeof(iso_bin)) < 0) {
        log_line("genisoimage not found");
        status_write("RIP|stage=failed|reason=genisoimage not found");
        return EX_FAIL;
    }
    log_line("dvdbackup=%s", backup_bin);
    log_line("genisoimage=%s", iso_bin);

    snprintf(g_staging, sizeof(g_staging), "%s/%s/rip-%d", dvd_dir, STAGING_DIR,
             (int)getpid());
    mkdir_p(g_staging);
    unlink(g_part_path);

    status_write("RIP|stage=backup|pct=0|bytes=0|total=%" PRIu64
                 "|msg=Creating decrypted backup",
                 info.payload);
    {
        char *av[] = {
            backup_bin,
            "-i", (char *)dev,
            "-M",
            "-o", g_staging,
            "-n", info.volume,
            "-r", "a",
            "-p",
            NULL
        };
        rc = run_tool(av, "backup", "Creating decrypted backup", g_staging,
                      info.payload, 0, 50);
    }
    if (g_cancel)
        rc = EX_CANCEL;
    if (rc == EX_CANCEL) {
        log_line("cancelled during backup");
        cleanup_partial();
        status_write("RIP|stage=cancelled");
        return EX_CANCEL;
    }
    if (rc != 0) {
        log_line("dvdbackup exit %d", rc);
        cleanup_partial();
        status_write("RIP|stage=failed|reason=Disc read error");
        return EX_READ;
    }
    if (find_videots_root(g_staging, dvdroot, sizeof(dvdroot)) < 0) {
        log_line("VIDEO_TS missing after backup");
        cleanup_partial();
        status_write("RIP|stage=failed|reason=Backup incomplete");
        return EX_FAIL;
    }
    nfiles = validate_tree_files(dvdroot, &tree_bytes);
    log_line("backup tree files=%d bytes=%" PRIu64 " root=%s", nfiles,
             tree_bytes, dvdroot);
    if (nfiles < 3 || tree_bytes < 1024ULL * 1024ULL) {
        cleanup_partial();
        status_write("RIP|stage=failed|reason=Backup incomplete");
        return EX_FAIL;
    }

    status_write("RIP|stage=iso|pct=50|bytes=0|total=%" PRIu64
                 "|msg=Building ISO",
                 tree_bytes);
    {
        char *av[] = {
            iso_bin,
            "-dvd-video",
            "-V", info.volume,
            "-o", g_part_path,
            dvdroot,
            NULL
        };
        rc = run_tool(av, "iso", "Building ISO", g_part_path, tree_bytes, 50,
                      98);
    }
    if (g_cancel)
        rc = EX_CANCEL;
    if (rc == EX_CANCEL) {
        log_line("cancelled during ISO");
        cleanup_partial();
        status_write("RIP|stage=cancelled");
        return EX_CANCEL;
    }
    if (rc != 0) {
        log_line("genisoimage exit %d", rc);
        cleanup_partial();
        status_write("RIP|stage=failed|reason=ISO build failed");
        return EX_FAIL;
    }

    status_write("RIP|stage=validate|pct=98|msg=Validating ISO");
    if (stat(g_part_path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 1024 * 1024) {
        log_line("ISO part missing or tiny");
        cleanup_partial();
        status_write("RIP|stage=failed|reason=ISO build failed");
        return EX_VALIDATE;
    }
    iso_size = (uint64_t)st.st_size;
    if (tree_bytes > 0 && iso_size + 2048ULL * 64ULL < tree_bytes) {
        log_line("ISO truncated: iso=%" PRIu64 " tree=%" PRIu64, iso_size,
                 tree_bytes);
        cleanup_partial();
        status_write("RIP|stage=failed|reason=ISO build failed");
        return EX_VALIDATE;
    }
    {
        char vname[64];
        if (!dvd_library_validate_iso(g_part_path, vname, sizeof(vname))) {
            log_line("IFO validation failed");
            cleanup_partial();
            status_write("RIP|stage=failed|reason=ISO validation failed");
            return EX_VALIDATE;
        }
        log_line("validate ok display='%s' size=%" PRIu64, vname, iso_size);
    }
    {
        int fd = open(g_part_path, O_RDONLY);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
    if (rename(g_part_path, g_iso_path) != 0) {
        log_line("rename failed: %s", strerror(errno));
        cleanup_partial();
        status_write("RIP|stage=failed|reason=Could not finish ISO");
        return EX_FAIL;
    }
    g_part_path[0] = 0;
    rm_rf(g_staging);
    g_staging[0] = 0;
    dvd_library_invalidate_cache();
    log_line("complete path=%s size=%" PRIu64, g_iso_path, iso_size);
    status_write("RIP|stage=complete|pct=100|path=%s|bytes=%" PRIu64
                 "|msg=Rip complete",
                 g_iso_path, iso_size);
    return EX_OK;
}

static void usage(void)
{
    fprintf(stderr,
            "Usage:\n"
            "  dvd_rip_iso probe [device]\n"
            "  dvd_rip_iso list-usb\n"
            "  dvd_rip_iso rip [device] [usb-root]\n"
            "    --status-file PATH   (default %s)\n",
            STATUS_DEFAULT);
}

int main(int argc, char **argv)
{
    const char *cmd = NULL;
    const char *dev = SR0_DEFAULT;
    const char *usb = NULL;
    int i, rc;
    int have_dev = 0;

    setvbuf(stderr, NULL, _IONBF, 0);
    snprintf(g_status_path, sizeof(g_status_path), "%s", STATUS_DEFAULT);
    resolve_selfdir(argc > 0 ? argv[0] : NULL);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGHUP, on_sig);
    signal(SIGPIPE, SIG_IGN);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--status-file") && i + 1 < argc) {
            snprintf(g_status_path, sizeof(g_status_path), "%s", argv[++i]);
        } else if (!cmd) {
            cmd = argv[i];
        } else if (!strcmp(cmd, "probe") && !have_dev) {
            dev = argv[i];
            have_dev = 1;
        } else if (!strcmp(cmd, "rip") && !have_dev) {
            dev = argv[i];
            have_dev = 1;
        } else if (!strcmp(cmd, "rip") && !usb) {
            usb = argv[i];
        } else {
            usage();
            return EX_FAIL;
        }
    }
    if (!cmd) {
        usage();
        return EX_FAIL;
    }
    if (!strcmp(cmd, "probe"))
        return cmd_probe(dev);
    if (!strcmp(cmd, "list-usb"))
        return cmd_list_usb();
    if (!strcmp(cmd, "rip")) {
        if (!usb) {
            UsbDest u[8];
            int n = list_usb(u, 8);
            int w = 0, wi = -1;
            for (i = 0; i < n; i++) {
                if (u[i].writable) {
                    w++;
                    wi = i;
                }
            }
            if (w != 1) {
                fprintf(stderr, "RIP: pass USB root (found %d writable)\n", w);
                return EX_NOUSB;
            }
            usb = u[wi].path;
        }
        rc = cmd_rip(dev, usb);
        if (g_log) {
            fclose(g_log);
            g_log = NULL;
        }
        return rc;
    }
    usage();
    return EX_FAIL;
}
