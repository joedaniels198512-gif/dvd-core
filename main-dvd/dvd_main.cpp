#include <stdio.h>
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

/*
 * DVD launcher supervision for MiSTer_DVD.
 *
 * /tmp/dvd_launcher.pid is written by dvd_launcher itself after it takes an
 * exclusive flock on the file. The file therefore contains a real PID, not
 * just a lock token. This module never flocks that file: taking the lock
 * would compete with the launcher for singleton ownership.
 *
 * Identifying a live launcher:
 *   1. Read the pidfile, then confirm /proc/<pid> is alive and cmdline
 *      contains "dvd_launcher" but not "dvd_autostart_daemon".
 *   2. If the pidfile is stale (dead PID, recycled PID, missing file),
 *      scan /proc for a matching cmdline.
 *
 * A launcher this process spawned can be reaped with waitpid(WNOHANG).
 * An inherited/orphaned launcher (survived a previous Main crash; PPID 1)
 * is not waitpid-eligible; poll uses kill(pid,0) + /proc instead.
 */

#define DVD_LAUNCHER_PATH   "/media/fat/DVD/dev/dvd_launcher"
#define DVD_LIB_DIR         "/media/fat/DVD/lib"
#define DVD_LOG_DIR         "/media/fat/DVD/logs"
#define DVD_LAUNCHER_LOG    DVD_LOG_DIR "/launcher.log"
#define DVD_LAUNCHER_PREV   DVD_LOG_DIR "/launcher.previous.log"
#define DVD_LAUNCHER_PIDFILE "/tmp/dvd_launcher.pid"

#define TERM_WAIT_SEC       8
#define KILL_WAIT_SEC       2
#define RESPAWN_WINDOW_SEC  5
#define RESPAWN_MAX         3
#define POLL_INTERVAL_MS    500

static int     g_session;
static int     g_owned;          /* 1 = waitpid-eligible child of this process */
static pid_t   g_launcher_pid = -1;
static int     g_halt_respawn;
static int     g_fail_count;
static time_t  g_last_start_ts;

static int exe_is_mister_dvd(void)
{
	const char *p = getappname();
	const char *base;

	if (!p || !p[0])
		return 0;
	base = strrchr(p, '/');
	base = base ? base + 1 : p;
	return strcasecmp(base, "MiSTer_DVD") == 0;
}

static int name_is_dvd_player(const char *n)
{
	return n && n[0] && strcasecmp(n, "DVD-Player") == 0;
}

static int core_is_dvd_player(void)
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
	return errno == EPERM; /* exists, not signalable to us */
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

static int cmdline_is_launcher(const char *cmd)
{
	if (!cmd || !strstr(cmd, "dvd_launcher"))
		return 0;
	if (strstr(cmd, "dvd_autostart_daemon"))
		return 0;
	return 1;
}

static int cmdline_is_player(const char *cmd)
{
	return cmd && strstr(cmd, "dvd_av_threaded_test") != NULL;
}

static int pid_is_launcher(pid_t pid)
{
	char cmd[512];

	if (!pid_alive(pid))
		return 0;
	if (read_cmdline(pid, cmd, sizeof(cmd)) < 0)
		return 0;
	return cmdline_is_launcher(cmd);
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

static pid_t scan_proc_launcher(void)
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
		if (cmdline_is_launcher(cmd))
		{
			found = (pid_t)v;
			break;
		}
	}
	closedir(d);
	return found;
}

static pid_t find_live_launcher(void)
{
	pid_t pid = read_pidfile(DVD_LAUNCHER_PIDFILE);
	if (pid_is_launcher(pid))
		return pid;
	return scan_proc_launcher();
}

static void rotate_launcher_log(void)
{
	struct stat st;

	mkdir(DVD_LOG_DIR, 0755);
	if (stat(DVD_LAUNCHER_LOG, &st) == 0)
		rename(DVD_LAUNCHER_LOG, DVD_LAUNCHER_PREV);
}

static void adopt_launcher(pid_t pid, int owned)
{
	g_launcher_pid = pid;
	g_owned = owned;
	printf("DVD_MAIN: using launcher pid=%ld owned=%d\n", (long)pid, owned);
}

static int spawn_launcher(int rotate)
{
	pid_t pid;
	const char *old;
	char ld[512];

	if (access(DVD_LAUNCHER_PATH, X_OK) != 0)
	{
		printf("DVD_MAIN: launcher missing: %s (%s)\n",
		       DVD_LAUNCHER_PATH, strerror(errno));
		return -1;
	}

	mkdir(DVD_LOG_DIR, 0755);
	if (rotate)
		rotate_launcher_log();

	pid = fork();
	if (pid < 0)
	{
		printf("DVD_MAIN: fork failed: %s\n", strerror(errno));
		return -1;
	}
	if (pid == 0)
	{
		int fd;

		old = getenv("LD_LIBRARY_PATH");
		if (old && old[0])
			snprintf(ld, sizeof(ld), "%s:%s", DVD_LIB_DIR, old);
		else
			snprintf(ld, sizeof(ld), "%s", DVD_LIB_DIR);
		setenv("LD_LIBRARY_PATH", ld, 1);

		fd = open("/dev/null", O_RDONLY);
		if (fd >= 0)
		{
			dup2(fd, STDIN_FILENO);
			if (fd != STDIN_FILENO)
				close(fd);
		}
		fd = open(DVD_LAUNCHER_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0)
		{
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
				close(fd);
		}

		execl(DVD_LAUNCHER_PATH, "dvd_launcher", (char *)NULL);
		_exit(127);
	}

	g_last_start_ts = time(NULL);
	adopt_launcher(pid, 1);
	printf("DVD_MAIN: launcher started pid=%ld\n", (long)pid);
	return 0;
}

static int wait_pid_gone(pid_t pid, int seconds)
{
	int i;

	for (i = 0; i < seconds * 10; i++)
	{
		if (!pid_is_launcher(pid))
			return 1;
		if (g_owned && pid == g_launcher_pid)
		{
			int st;
			pid_t r = waitpid(pid, &st, WNOHANG);
			if (r == pid)
			{
				g_owned = 0;
				g_launcher_pid = -1;
				return 1;
			}
		}
		usleep(100000);
	}
	return !pid_alive(pid);
}

static void signal_matching(int sig, int launchers, int players)
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
		if ((launchers && cmdline_is_launcher(cmd)) ||
		    (players && cmdline_is_player(cmd)))
		{
			kill((pid_t)v, sig);
		}
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

	pid = g_launcher_pid;
	if (!pid_is_launcher(pid))
		pid = find_live_launcher();

	if (pid_is_launcher(pid))
	{
		printf("DVD_MAIN: stopping launcher pid=%ld\n", (long)pid);
		kill(pid, SIGTERM);
		if (!wait_pid_gone(pid, TERM_WAIT_SEC))
		{
			printf("DVD_MAIN: SIGKILL launcher pid=%ld\n", (long)pid);
			kill(pid, SIGKILL);
			wait_pid_gone(pid, KILL_WAIT_SEC);
		}
	}

	if (g_owned && g_launcher_pid > 0)
	{
		int st;
		waitpid(g_launcher_pid, &st, WNOHANG);
	}

	g_owned = 0;
	g_launcher_pid = -1;
	orphan_sweep();
	printf("DVD_MAIN: DVD processes stopped\n");
}

static int launcher_running(void)
{
	if (pid_is_launcher(g_launcher_pid))
		return 1;
	{
		pid_t live = find_live_launcher();
		if (live > 0)
		{
			/* Inherited after a Main crash, or pid recycled into a
			 * new legitimate launcher. Not waitpid-eligible unless
			 * this process spawned it. */
			adopt_launcher(live, 0);
			return 1;
		}
	}
	g_launcher_pid = -1;
	g_owned = 0;
	return 0;
}

static void reap_owned(void)
{
	int st;
	pid_t r;

	if (!g_owned || g_launcher_pid <= 0)
		return;
	r = waitpid(g_launcher_pid, &st, WNOHANG);
	if (r == g_launcher_pid)
	{
		printf("DVD_MAIN: launcher pid=%ld exited status=%d\n",
		       (long)r, st);
		g_owned = 0;
		g_launcher_pid = -1;
	}
}

static void start_or_adopt(void)
{
	pid_t live = find_live_launcher();
	if (live > 0)
	{
		int owned = g_owned && (live == g_launcher_pid);
		adopt_launcher(live, owned);
		return;
	}
	if (g_halt_respawn)
		return;
	spawn_launcher(g_last_start_ts == 0);
}

void dvd_main_on_core_ready(void)
{
	if (!exe_is_mister_dvd())
		return;

	if (!core_is_dvd_player())
	{
		if (g_session || pid_is_launcher(g_launcher_pid) || find_live_launcher() > 0)
			dvd_main_stop_all();
		return;
	}

	if (!g_session)
	{
		g_session = 1;
		g_halt_respawn = 0;
		g_fail_count = 0;
		g_last_start_ts = 0;
		printf("DVD_MAIN: DVD-Player session begin\n");
	}
	start_or_adopt();
}

void dvd_main_poll(void)
{
	time_t now;
	static struct timespec last_check;
	struct timespec ts;
	long dt_ms;

	if (!exe_is_mister_dvd())
		return;
	if (!g_session)
		return;
	if (!core_is_dvd_player())
	{
		dvd_main_stop_all();
		return;
	}

	reap_owned();

	/*
	 * user_io_poll runs every scheduler co_poll slice (~frame rate).
	 * Identity / respawn checks match the old daemon's 0.5 s cadence so
	 * we do not walk /proc on every frame. waitpid reap stays above.
	 */
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
	{
		if (last_check.tv_sec)
		{
			dt_ms = (ts.tv_sec - last_check.tv_sec) * 1000L +
				(ts.tv_nsec - last_check.tv_nsec) / 1000000L;
			if (dt_ms >= 0 && dt_ms < POLL_INTERVAL_MS &&
			    pid_is_launcher(g_launcher_pid))
				return;
		}
		last_check = ts;
	}

	if (launcher_running())
		return;
	if (g_halt_respawn)
		return;

	printf("DVD_MAIN: launcher exited unexpectedly\n");
	now = time(NULL);
	if (g_last_start_ts > 0 && (now - g_last_start_ts) <= RESPAWN_WINDOW_SEC)
		g_fail_count++;
	else
		g_fail_count = 1;

	if (g_fail_count > RESPAWN_MAX)
	{
		printf("DVD_MAIN: respawn halted (%d starts in %d seconds)\n",
		       g_fail_count, RESPAWN_WINDOW_SEC);
		g_halt_respawn = 1;
		return;
	}

	printf("DVD_MAIN: restart %d/%d\n", g_fail_count, RESPAWN_MAX);
	spawn_launcher(0);
}
