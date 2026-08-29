/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dvd_appliance.c — physical-DVD autoplay supervisor for
 * DVD Player - Appliance. No launcher UI, no ISO library, no ripper.
 *
 * State machine:
 *   NO_DISC → DISC_DETECTED → PLAYING → WAIT_EJECT → NO_DISC
 *
 * Hold CANCEL/B in the player still exits playback. This supervisor
 * will not relaunch the same inserted disc.
 */

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#define _GNU_SOURCE

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/ifo_types.h>

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/cdrom.h>
#endif

#ifndef CDROM_DRIVE_STATUS
#define CDROM_DRIVE_STATUS 0x5326
#endif
#ifndef CDROM_DISC_STATUS
#define CDROM_DISC_STATUS  0x5327
#endif
#ifndef CDS_NO_DISC
#define CDS_NO_DISC          1
#define CDS_TRAY_OPEN        2
#define CDS_DRIVE_NOT_READY  3
#define CDS_DISC_OK          4
#endif
#ifndef CDS_NO_INFO
#define CDS_NO_INFO          0
#define CDS_AUDIO            100
#define CDS_DATA_1           101
#define CDS_DATA_2           102
#define CDS_MIXED            105
#endif
#ifndef CDSL_CURRENT
#define CDSL_CURRENT ((int)(~0U >> 1))
#endif

#define SR0_PATH        "/dev/sr0"
#define APPLIANCE_ROOT  "/media/fat/DVD_Appliance"
#define APPLIANCE_BIN   APPLIANCE_ROOT "/bin"
#define APPLIANCE_LOG   APPLIANCE_ROOT "/logs"
#define PLAYER_NAME     "dvd_av_threaded_test"
#define PID_FILE        "/tmp/dvd_appliance.pid"
#define POLL_US         1000000
#define PLAYER_TERM_SEC 8

enum {
	ST_NO_DISC = 0,
	ST_PLAYING,
	ST_WAIT_EJECT
};

static volatile sig_atomic_t g_stop;
static pid_t g_child = -1;
static int g_pid_fd = -1;
static int g_state = ST_NO_DISC;
static int g_last_kind = -1; /* last disc classify: -1 none, 0 empty, 1 dvd, 2 other */

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
	if (g_child > 0)
		kill(g_child, SIGTERM);
}

static void log_line(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	fflush(stderr);
}

static void write_pidfile(void)
{
	char buf[32];
	int n;

	g_pid_fd = open(PID_FILE, O_RDWR | O_CREAT, 0644);
	if (g_pid_fd < 0)
		return;
	if (flock(g_pid_fd, LOCK_EX | LOCK_NB) < 0) {
		close(g_pid_fd);
		g_pid_fd = -1;
		fprintf(stderr, "APPLIANCE: another supervisor holds %s\n",
		        PID_FILE);
		exit(1);
	}
	if (ftruncate(g_pid_fd, 0) == 0) {
		n = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
		if (n > 0)
			(void)write(g_pid_fd, buf, (size_t)n);
	}
}

static void clear_pidfile(void)
{
	if (g_pid_fd >= 0) {
		flock(g_pid_fd, LOCK_UN);
		close(g_pid_fd);
		g_pid_fd = -1;
	}
	unlink(PID_FILE);
}

static void dvdread_quiet(void *p, dvd_logger_level_t level, const char *fmt,
                          va_list ap)
{
	(void)p;
	(void)level;
	(void)fmt;
	(void)ap;
}

static int drive_status(void)
{
	int fd, st;

	if (access(SR0_PATH, F_OK) != 0)
		return CDS_NO_DISC;
	fd = open(SR0_PATH, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return CDS_NO_DISC;
	st = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
	close(fd);
	if (st < 0)
		return CDS_DRIVE_NOT_READY;
	return st;
}

static int disc_status(void)
{
	int fd, st;

	fd = open(SR0_PATH, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return CDS_NO_INFO;
	st = ioctl(fd, CDROM_DISC_STATUS, CDSL_CURRENT);
	close(fd);
	if (st < 0)
		return CDS_NO_INFO;
	return st;
}

/* 0 = no/empty, 1 = DVD-Video, 2 = present but not DVD-Video */
static int classify_media(void)
{
	static const dvd_logger_cb quiet = { .pf_log = dvdread_quiet };
	dvd_reader_t *dvd;
	ifo_handle_t *ifo;
	int ds, kind = 2;

	if (drive_status() != CDS_DISC_OK)
		return 0;
	ds = disc_status();
	if (ds == CDS_NO_DISC || ds == CDS_NO_INFO)
		return 0;
	if (ds == CDS_AUDIO)
		return 2;

	dvd = DVDOpen2(NULL, &quiet, SR0_PATH);
	if (!dvd)
		return 2;
	ifo = ifoOpenVMGI(dvd);
	if (ifo && ifo->vmgi_mat &&
	    memcmp(ifo->vmgi_mat->vmg_identifier, "DVDVIDEO-VMG", 12) == 0 &&
	    ifoRead_TT_SRPT(ifo) && ifo->tt_srpt &&
	    ifo->tt_srpt->nr_of_srpts >= 1)
		kind = 1;
	if (ifo)
		ifoClose(ifo);
	DVDClose(dvd);
	return kind;
}

static void player_path(char *out, size_t cap)
{
	snprintf(out, cap, "%s/%s", APPLIANCE_BIN, PLAYER_NAME);
}

static int launch_player(void)
{
	char player[512];
	pid_t pid;
	int fd;
	char logpath[512];

	player_path(player, sizeof(player));
	if (access(player, X_OK) != 0) {
		fprintf(stderr, "APPLIANCE: player missing %s (%s)\n",
		        player, strerror(errno));
		return -1;
	}

	mkdir(APPLIANCE_LOG, 0755);
	snprintf(logpath, sizeof(logpath), "%s/player.log", APPLIANCE_LOG);

	fprintf(stderr, "APPLIANCE: launching player %s\n", SR0_PATH);
	fflush(stderr);
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "APPLIANCE: fork failed: %s\n", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
				close(fd);
		}
		execl(player, PLAYER_NAME, SR0_PATH,
		      "--buffered-yuv-video",
		      "--fpga-yuv420",
		      "--fpga-yuv420-subtitles",
		      "--initial-video-skip", "1",
		      "--video-advance-ms", "20",
		      "--authored-start",
		      (char *)NULL);
		fprintf(stderr, "APPLIANCE: exec failed: %s\n", strerror(errno));
		_exit(127);
	}
	g_child = pid;
	return 0;
}

static int reap_player(int *status_out)
{
	int st = 0;
	pid_t w;

	if (g_child <= 0)
		return 0;
	w = waitpid(g_child, &st, WNOHANG);
	if (w == 0)
		return 0;
	if (w == g_child || (w < 0 && errno != EINTR)) {
		if (status_out)
			*status_out = st;
		g_child = -1;
		return 1;
	}
	return 0;
}

static void stop_player(void)
{
	int i, st;

	if (g_child <= 0)
		return;
	kill(g_child, SIGTERM);
	for (i = 0; i < PLAYER_TERM_SEC * 10; i++) {
		if (waitpid(g_child, &st, WNOHANG) == g_child) {
			g_child = -1;
			return;
		}
		usleep(100000);
	}
	kill(g_child, SIGKILL);
	waitpid(g_child, &st, 0);
	g_child = -1;
}

static void log_classify(int kind)
{
	if (kind == g_last_kind)
		return;
	g_last_kind = kind;
	if (kind == 0)
		log_line("APPLIANCE: no disc");
	else if (kind == 1)
		fprintf(stderr, "APPLIANCE: DVD detected %s\n", SR0_PATH);
	else
		log_line("APPLIANCE: unsupported media (not DVD-Video)");
	fflush(stderr);
}

int main(void)
{
	struct sigaction sa;

	mkdir(APPLIANCE_ROOT, 0755);
	mkdir(APPLIANCE_BIN, 0755);
	mkdir(APPLIANCE_LOG, 0755);
	mkdir(APPLIANCE_ROOT "/config", 0755);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	write_pidfile();
	log_line("APPLIANCE: supervisor start");

	while (!g_stop) {
		int kind, st;

		if (g_state == ST_PLAYING) {
			if (reap_player(&st)) {
				fprintf(stderr,
				        "APPLIANCE: player exited status=%d\n",
				        st);
				fflush(stderr);
				if (classify_media() == 0) {
					g_state = ST_NO_DISC;
					g_last_kind = -1;
					log_line("APPLIANCE: disc removed - autoplay rearmed");
				} else {
					g_state = ST_WAIT_EJECT;
					log_line("APPLIANCE: suppress relaunch until eject");
				}
			}
			usleep(200000);
			continue;
		}

		kind = classify_media();
		log_classify(kind);

		if (g_state == ST_NO_DISC) {
			if (kind == 1) {
				if (launch_player() == 0)
					g_state = ST_PLAYING;
				else
					usleep(POLL_US);
				continue;
			}
		} else if (g_state == ST_WAIT_EJECT) {
			if (kind == 0) {
				g_state = ST_NO_DISC;
				g_last_kind = -1;
				log_line("APPLIANCE: disc removed - autoplay rearmed");
			}
		}
		usleep(POLL_US);
	}

	stop_player();
	log_line("APPLIANCE: supervisor stop");
	clear_pidfile();
	return 0;
}
