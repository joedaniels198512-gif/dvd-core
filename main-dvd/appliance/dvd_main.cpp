/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "dvd_main.h"
#include "fpga_io.h"
#include "user_io.h"
#include "file_io.h"

/*
 * Appliance supervisor for MiSTer_DVD_Appliance.
 *
 * /tmp/dvd_appliance.pid is written by dvd_appliance after it takes an
 * exclusive flock. This module never flocks that file.
 *
 * Does not start or supervise /media/fat/DVD/dev/dvd_launcher.
 */

#define DVD_APPLIANCE_PATH    "/media/fat/DVD_Appliance/bin/dvd_appliance"
#define DVD_APPLIANCE_LIB_DIR "/media/fat/DVD_Appliance/lib"
#define DVD_LIB_DIR           "/media/fat/DVD/lib"
#define DVD_APPLIANCE_LOG_DIR "/media/fat/DVD_Appliance/logs"
#define DVD_APPLIANCE_LOG     DVD_APPLIANCE_LOG_DIR "/appliance.log"
#define DVD_APPLIANCE_PREV    DVD_APPLIANCE_LOG_DIR "/appliance.previous.log"
#define DVD_APPLIANCE_PIDFILE "/tmp/dvd_appliance.pid"
#define DVD_APPLIANCE_CMD_FIFO "/tmp/dvd_appliance.cmd"
#define DVD_APPLIANCE_ISO_INDEX 0

#define TERM_WAIT_SEC       8
#define KILL_WAIT_SEC       2
#define RESPAWN_WINDOW_SEC  5
#define RESPAWN_MAX         3
#define POLL_INTERVAL_MS    500
#define LAUNCH_SETTLE_MS    500

enum {
	LAUNCH_NONE = 0,
	LAUNCH_STARTING,
	LAUNCH_RUNNING
};

static int     g_session;
static int     g_owned;
static pid_t   g_child_pid = -1;
static int     g_launch_state;
static int     g_halt_respawn;
static int     g_fail_count;
static time_t  g_last_start_ts;
static int     g_pending_launch;
static int64_t g_launch_not_before;

static int64_t mono_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

static int exe_is_mister_dvd_appliance(void)
{
	const char *p = getappname();
	const char *base;

	if (!p || !p[0])
		return 0;
	base = strrchr(p, '/');
	base = base ? base + 1 : p;
	return strcasecmp(base, "MiSTer_DVD_Appliance") == 0;
}

static int name_is_dvd_player(const char *n)
{
	return n && n[0] &&
	       (strcasecmp(n, "DVD-Player") == 0 ||
	        strcasecmp(n, "DVD-Player-Appliance") == 0);
}

static int core_is_appliance(void)
{
	return name_is_dvd_player(user_io_get_core_name(0)) ||
	       name_is_dvd_player(user_io_get_core_name(1));
}

static int pid_alive(pid_t pid)
{
	if (pid <= 1)
		return 0;
	if (kill(pid, 0) == 0)
		return 1;
	return errno == EPERM;
}

static int read_cmdline(pid_t pid, char *buf, size_t buflen)
{
	char path[64];
	int fd, n, i;

	if (buflen < 2)
		return -1;
	snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = (int)read(fd, buf, buflen - 1);
	close(fd);
	if (n <= 0)
		return -1;
	for (i = 0; i < n; i++)
	{
		if (buf[i] == '\0')
			buf[i] = ' ';
	}
	buf[n] = '\0';
	return 0;
}

static int cmdline_is_appliance(const char *cmd)
{
	return cmd && strstr(cmd, "dvd_appliance") != NULL;
}

static int cmdline_is_player(const char *cmd)
{
	return cmd && strstr(cmd, "dvd_av_threaded_test") != NULL;
}

static int pid_is_appliance(pid_t pid)
{
	char cmd[512];

	if (!pid_alive(pid))
		return 0;
	if (read_cmdline(pid, cmd, sizeof(cmd)) < 0)
		return 0;
	return cmdline_is_appliance(cmd);
}

static int owned_child_live(void)
{
	int st;
	pid_t r;

	if (!g_owned || g_child_pid <= 0)
		return 0;

	r = waitpid(g_child_pid, &st, WNOHANG);
	if (r == 0)
	{
		if (g_launch_state == LAUNCH_STARTING && pid_is_appliance(g_child_pid))
		{
			g_launch_state = LAUNCH_RUNNING;
			printf("APPLIANCE_MAIN: supervisor pid=%ld exec complete\n",
			       (long)g_child_pid);
		}
		else if (g_launch_state == LAUNCH_NONE)
			g_launch_state = LAUNCH_STARTING;
		return 1;
	}
	if (r == g_child_pid)
	{
		printf("APPLIANCE_MAIN: supervisor pid=%ld exited status=%d\n",
		       (long)r, st);
		g_owned = 0;
		g_child_pid = -1;
		g_launch_state = LAUNCH_NONE;
		return 0;
	}
	if (r < 0)
	{
		printf("APPLIANCE_MAIN: waitpid pid=%ld: %s\n",
		       (long)g_child_pid, strerror(errno));
		g_owned = 0;
		g_child_pid = -1;
		g_launch_state = LAUNCH_NONE;
		return 0;
	}
	return 0;
}

static pid_t read_pidfile(const char *path)
{
	char buf[64];
	int fd, n;
	char *end;
	long v;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	v = strtol(buf, &end, 10);
	if (end == buf || v <= 1 || v > (long)INT_MAX)
		return -1;
	return (pid_t)v;
}

static pid_t scan_proc_appliance(void)
{
	DIR *d;
	struct dirent *de;
	pid_t found = -1;

	d = opendir("/proc");
	if (!d)
		return -1;
	while ((de = readdir(d)) != NULL)
	{
		char *end;
		long v;
		char cmd[512];

		if (!de->d_name[0] || !isdigit((unsigned char)de->d_name[0]))
			continue;
		v = strtol(de->d_name, &end, 10);
		if (*end || v <= 1)
			continue;
		if (read_cmdline((pid_t)v, cmd, sizeof(cmd)) < 0)
			continue;
		if (cmdline_is_appliance(cmd))
		{
			found = (pid_t)v;
			break;
		}
	}
	closedir(d);
	return found;
}

static pid_t find_live_appliance(void)
{
	pid_t pid = read_pidfile(DVD_APPLIANCE_PIDFILE);
	if (pid_is_appliance(pid))
		return pid;
	return scan_proc_appliance();
}

static void rotate_appliance_log(void)
{
	struct stat st;

	mkdir("/media/fat/DVD_Appliance", 0755);
	mkdir(DVD_APPLIANCE_LOG_DIR, 0755);
	mkdir("/media/fat/games", 0755);
	mkdir("/media/fat/games/DVD-Player", 0755);
	if (stat(DVD_APPLIANCE_LOG, &st) == 0)
		rename(DVD_APPLIANCE_LOG, DVD_APPLIANCE_PREV);
}

static void adopt_appliance(pid_t pid, int owned)
{
	g_child_pid = pid;
	g_owned = owned;
	if (!owned)
		g_launch_state = LAUNCH_RUNNING;
	else if (g_launch_state == LAUNCH_NONE)
		g_launch_state = LAUNCH_RUNNING;
	printf("APPLIANCE_MAIN: using supervisor pid=%ld owned=%d state=%d\n",
	       (long)pid, owned, g_launch_state);
}

static int spawn_appliance(int rotate)
{
	pid_t pid;
	const char *old;
	char ld[512];

	if (owned_child_live())
	{
		printf("APPLIANCE_MAIN: spawn skipped, owned pid=%ld still live\n",
		       (long)g_child_pid);
		return 0;
	}

	if (access(DVD_APPLIANCE_PATH, X_OK) != 0)
	{
		printf("APPLIANCE_MAIN: supervisor missing: %s (%s)\n",
		       DVD_APPLIANCE_PATH, strerror(errno));
		return -1;
	}

	mkdir("/media/fat/DVD_Appliance", 0755);
	mkdir(DVD_APPLIANCE_LOG_DIR, 0755);
	mkdir("/media/fat/games", 0755);
	mkdir("/media/fat/games/DVD-Player", 0755);
	if (rotate)
		rotate_appliance_log();

	pid = fork();
	if (pid < 0)
	{
		printf("APPLIANCE_MAIN: fork failed: %s\n", strerror(errno));
		return -1;
	}
	if (pid == 0)
	{
		int fd;

		old = getenv("LD_LIBRARY_PATH");
		/* Appliance libs first. Existing DVD/lib is a read-only
		 * fallback (libdvdcss) and is never written. */
		if (old && old[0])
			snprintf(ld, sizeof(ld), "%s:%s:%s",
			         DVD_APPLIANCE_LIB_DIR, DVD_LIB_DIR, old);
		else
			snprintf(ld, sizeof(ld), "%s:%s",
			         DVD_APPLIANCE_LIB_DIR, DVD_LIB_DIR);
		setenv("LD_LIBRARY_PATH", ld, 1);

		fd = open("/dev/null", O_RDONLY);
		if (fd >= 0)
		{
			dup2(fd, STDIN_FILENO);
			if (fd != STDIN_FILENO)
				close(fd);
		}
		fd = open(DVD_APPLIANCE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0)
		{
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
				close(fd);
		}

		execl(DVD_APPLIANCE_PATH, DVD_APPLIANCE_PATH, (char *)NULL);
		_exit(127);
	}

	g_last_start_ts = time(NULL);
	g_launch_state = LAUNCH_STARTING;
	adopt_appliance(pid, 1);
	printf("APPLIANCE_MAIN: supervisor started pid=%ld (starting)\n", (long)pid);
	return 0;
}

static int wait_pid_gone(pid_t pid, int seconds)
{
	int i;

	for (i = 0; i < seconds * 10; i++)
	{
		if (g_owned && pid == g_child_pid)
		{
			int st;
			pid_t r = waitpid(pid, &st, WNOHANG);
			if (r == pid)
			{
				g_owned = 0;
				g_child_pid = -1;
				g_launch_state = LAUNCH_NONE;
				return 1;
			}
			if (r < 0 && errno != EINTR)
			{
				g_owned = 0;
				g_child_pid = -1;
				g_launch_state = LAUNCH_NONE;
				return 1;
			}
		}
		else if (!pid_alive(pid))
			return 1;
		usleep(100000);
	}
	if (g_owned && pid == g_child_pid)
	{
		int st;
		if (waitpid(pid, &st, WNOHANG) == pid)
		{
			g_owned = 0;
			g_child_pid = -1;
			g_launch_state = LAUNCH_NONE;
			return 1;
		}
	}
	return !pid_alive(pid);
}

static void signal_matching(int sig, int supervisors, int players)
{
	DIR *d;
	struct dirent *de;

	d = opendir("/proc");
	if (!d)
		return;
	while ((de = readdir(d)) != NULL)
	{
		char *end;
		long v;
		char cmd[512];

		if (!de->d_name[0] || !isdigit((unsigned char)de->d_name[0]))
			continue;
		v = strtol(de->d_name, &end, 10);
		if (*end || v <= 1 || v == (long)getpid())
			continue;
		if (read_cmdline((pid_t)v, cmd, sizeof(cmd)) < 0)
			continue;
		if ((supervisors && cmdline_is_appliance(cmd)) ||
		    (players && cmdline_is_player(cmd)))
			kill((pid_t)v, sig);
	}
	closedir(d);
}

static void orphan_sweep(void)
{
	signal_matching(SIGTERM, 1, 1);
	usleep(1000000);
	signal_matching(SIGKILL, 1, 1);
}

void dvd_main_stop_all(void)
{
	pid_t pid;

	g_session = 0;
	g_halt_respawn = 0;
	g_fail_count = 0;
	g_last_start_ts = 0;
	g_pending_launch = 0;
	g_launch_not_before = 0;

	pid = g_child_pid;
	if (g_owned && pid > 0)
	{
		printf("APPLIANCE_MAIN: stopping owned supervisor pid=%ld\n", (long)pid);
		kill(pid, SIGTERM);
		if (!wait_pid_gone(pid, TERM_WAIT_SEC))
		{
			printf("APPLIANCE_MAIN: SIGKILL supervisor pid=%ld\n", (long)pid);
			kill(pid, SIGKILL);
			wait_pid_gone(pid, KILL_WAIT_SEC);
		}
		if (g_owned && g_child_pid > 0)
		{
			int st;
			waitpid(g_child_pid, &st, WNOHANG);
		}
	}
	else
	{
		if (!pid_is_appliance(pid))
			pid = find_live_appliance();
		if (pid_is_appliance(pid))
		{
			printf("APPLIANCE_MAIN: stopping supervisor pid=%ld\n", (long)pid);
			kill(pid, SIGTERM);
			if (!wait_pid_gone(pid, TERM_WAIT_SEC))
			{
				printf("APPLIANCE_MAIN: SIGKILL supervisor pid=%ld\n", (long)pid);
				kill(pid, SIGKILL);
				wait_pid_gone(pid, KILL_WAIT_SEC);
			}
		}
	}

	g_owned = 0;
	g_child_pid = -1;
	g_launch_state = LAUNCH_NONE;
	orphan_sweep();
	printf("APPLIANCE_MAIN: Appliance processes stopped\n");
}

static int appliance_running(void)
{
	if (owned_child_live())
		return 1;
	{
		pid_t live = find_live_appliance();
		if (live > 0)
		{
			adopt_appliance(live, 0);
			return 1;
		}
	}
	g_child_pid = -1;
	g_owned = 0;
	g_launch_state = LAUNCH_NONE;
	return 0;
}

static void reap_owned(void)
{
	owned_child_live();
}

static void start_or_adopt(void)
{
	if (owned_child_live())
		return;
	{
		pid_t live = find_live_appliance();
		if (live > 0)
		{
			adopt_appliance(live, 0);
			return;
		}
	}
	if (g_halt_respawn)
		return;
	spawn_appliance(g_last_start_ts == 0);
}

void dvd_main_on_core_ready(void)
{
	if (!exe_is_mister_dvd_appliance())
		return;

	if (!core_is_appliance())
	{
		if (g_session || (g_owned && g_child_pid > 0) ||
		    pid_is_appliance(g_child_pid) || find_live_appliance() > 0)
			dvd_main_stop_all();
		return;
	}

	if (!g_session)
	{
		g_session = 1;
		g_halt_respawn = 0;
		g_fail_count = 0;
		g_last_start_ts = 0;
		printf("APPLIANCE_MAIN: DVD-Player session begin\n");
	}

	if (owned_child_live())
	{
		g_pending_launch = 0;
		return;
	}
	{
		pid_t live = find_live_appliance();
		if (live > 0)
		{
			adopt_appliance(live, 0);
			g_pending_launch = 0;
			return;
		}
	}
	g_pending_launch = 1;
	g_launch_not_before = mono_ms() + LAUNCH_SETTLE_MS;
	printf("APPLIANCE_MAIN: supervisor start armed (settle %d ms)\n",
	       LAUNCH_SETTLE_MS);
}

void dvd_main_poll(void)
{
	time_t now;
	static struct timespec last_check;
	struct timespec ts;
	long dt_ms;

	if (!exe_is_mister_dvd_appliance())
		return;
	if (!g_session)
		return;
	if (!core_is_appliance())
	{
		dvd_main_stop_all();
		return;
	}

	reap_owned();

	if (g_pending_launch)
	{
		if (mono_ms() < g_launch_not_before)
			return;
		g_pending_launch = 0;
		printf("APPLIANCE_MAIN: settle delay elapsed, starting supervisor\n");
		start_or_adopt();
		return;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
	{
		if (last_check.tv_sec)
		{
			dt_ms = (ts.tv_sec - last_check.tv_sec) * 1000L +
				(ts.tv_nsec - last_check.tv_nsec) / 1000000L;
			if (dt_ms >= 0 && dt_ms < POLL_INTERVAL_MS &&
			    (owned_child_live() || pid_is_appliance(g_child_pid)))
				return;
		}
		last_check = ts;
	}

	if (appliance_running())
		return;
	if (g_halt_respawn)
		return;

	printf("APPLIANCE_MAIN: supervisor exited unexpectedly\n");
	now = time(NULL);
	if (g_last_start_ts > 0 && (now - g_last_start_ts) <= RESPAWN_WINDOW_SEC)
		g_fail_count++;
	else
		g_fail_count = 1;

	if (g_fail_count > RESPAWN_MAX)
	{
		printf("APPLIANCE_MAIN: respawn halted (%d starts in %d seconds)\n",
		       g_fail_count, RESPAWN_WINDOW_SEC);
		g_halt_respawn = 1;
		return;
	}

	printf("APPLIANCE_MAIN: restart %d/%d\n", g_fail_count, RESPAWN_MAX);
	spawn_appliance(0);
}

static int path_is_iso(const char *p)
{
	const char *dot;

	if (!p || !p[0])
		return 0;
	dot = strrchr(p, '.');
	return dot && strcasecmp(dot, ".iso") == 0;
}

static void osd_log(const char *msg)
{
	printf("%s", msg);
	fflush(stdout);
	FILE *f = fopen(DVD_APPLIANCE_LOG, "a");
	if (f)
	{
		fputs(msg, f);
		fclose(f);
	}
}

int dvd_appliance_handle_iso_select(const char *sel_path, int ioctl_index)
{
	char full[PATH_MAX];
	char resolved[PATH_MAX];
	char msg[PATH_MAX + 96];
	char line[PATH_MAX + 32];
	const char *gp;
	struct stat st;
	int fd, n, wr;

	if (!exe_is_mister_dvd_appliance())
		return 0;
	if (!core_is_appliance())
		return 0;

	/*
	 * Never let this core fall through to user_io_file_tx. A DVD ISO
	 * must stay a Linux file; FPGA upload is not acceptable.
	 */
	if (ioctl_index != DVD_APPLIANCE_ISO_INDEX)
	{
		snprintf(msg, sizeof(msg),
		         "APPLIANCE OSD: ignore selector index=%d (Play ISO is F0)\n",
		         ioctl_index);
		osd_log(msg);
		return 1;
	}
	if (!sel_path || !sel_path[0])
	{
		osd_log("APPLIANCE OSD: empty ISO selection\n");
		return 1;
	}

	/*
	 * Copy out of getFullPath()'s static buffer immediately. Any later
	 * file_io call (including HomeDir / prefixGameDir) overwrites it.
	 */
	gp = getFullPath(sel_path);
	snprintf(full, sizeof(full), "%s", (gp && gp[0]) ? gp : sel_path);
	if (realpath(full, resolved))
		snprintf(full, sizeof(full), "%s", resolved);

	snprintf(msg, sizeof(msg), "APPLIANCE OSD: ISO selected path=%s\n", full);
	osd_log(msg);

	if (!path_is_iso(full))
	{
		snprintf(msg, sizeof(msg),
		         "APPLIANCE OSD: reject (not .iso) path=%s\n", full);
		osd_log(msg);
		return 1;
	}
	if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || access(full, R_OK) != 0)
	{
		snprintf(msg, sizeof(msg),
		         "APPLIANCE OSD: reject (not a readable file) path=%s (%s)\n",
		         full, strerror(errno));
		osd_log(msg);
		return 1;
	}

	if (mkfifo(DVD_APPLIANCE_CMD_FIFO, 0666) < 0 && errno != EEXIST)
	{
		snprintf(msg, sizeof(msg), "APPLIANCE OSD: mkfifo %s: %s\n",
		         DVD_APPLIANCE_CMD_FIFO, strerror(errno));
		osd_log(msg);
	}

	n = snprintf(line, sizeof(line), "PLAY_ISO %s\n", full);
	if (n < 0 || n >= (int)sizeof(line))
	{
		snprintf(msg, sizeof(msg),
		         "APPLIANCE OSD: path too long, not sent path=%s\n", full);
		osd_log(msg);
		return 1;
	}

	fd = open(DVD_APPLIANCE_CMD_FIFO, O_WRONLY | O_NONBLOCK);
	if (fd < 0)
	{
		snprintf(msg, sizeof(msg),
		         "APPLIANCE OSD: cannot send to supervisor (%s). "
		         "ISO was NOT transferred to FPGA.\n",
		         strerror(errno));
		osd_log(msg);
		return 1;
	}
	wr = (int)write(fd, line, (size_t)n);
	close(fd);
	if (wr != n)
	{
		snprintf(msg, sizeof(msg),
		         "APPLIANCE OSD: short FIFO write %d/%d (%s)\n",
		         wr, n, strerror(errno));
		osd_log(msg);
	}
	else
	{
		snprintf(msg, sizeof(msg),
		         "APPLIANCE OSD: ISO path sent (no FPGA transfer) path=%s\n",
		         full);
		osd_log(msg);
	}
	return 1;
}
