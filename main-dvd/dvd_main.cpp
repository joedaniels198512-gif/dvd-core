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

/*
 * DVD launcher supervision for MiSTer_DVD.
 *
 * /tmp/dvd_launcher.pid is written by dvd_launcher itself after it takes an
 * exclusive flock on the file. The file therefore contains a real PID, not
 * just a lock token. This module never flocks that file: taking the lock
 * would compete with the launcher for singleton ownership.
 *
 * Identifying a live launcher:
 *   - A child this process fork()ed is STARTING/RUNNING immediately. Liveness
 *     is waitpid(child, WNOHANG). Do not require /proc/cmdline to already
 *     show dvd_launcher (that lags until execl completes).
 *   - Adopted/orphaned launchers, stale pidfiles, and non-children: read
 *     the pidfile, then confirm /proc/<pid> is alive and cmdline contains
 *     "dvd_launcher" but not "dvd_autostart_daemon". If the pidfile is
 *     stale, scan /proc for a matching cmdline.
 *
 * A launcher this process spawned is reaped with waitpid(WNOHANG).
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
#define LAUNCH_SETTLE_MS    500

enum {
	LAUNCH_NONE = 0,
	LAUNCH_STARTING, /* fork() succeeded; execl may not have completed */
	LAUNCH_RUNNING
};

static int     g_session;
static int     g_owned;          /* 1 = waitpid-eligible child of this process */
static pid_t   g_launcher_pid = -1;
static int     g_launch_state;
static int     g_halt_respawn;
static int     g_fail_count;
static time_t  g_last_start_ts;
static int     g_pending_launch;
static int64_t g_launch_not_before; /* CLOCK_MONOTONIC ms deadline */

static int64_t mono_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

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

/*
 * Authoritative liveness for a child this process fork()ed.
 * Do not require /proc/cmdline to already show dvd_launcher: after fork
 * the child still appears as MiSTer_DVD until execl completes.
 */
static int owned_child_live(void)
{
	int st;
	pid_t r;

	if (!g_owned || g_launcher_pid <= 0)
		return 0;

	r = waitpid(g_launcher_pid, &st, WNOHANG);
	if (r == 0)
	{
		if (g_launch_state == LAUNCH_STARTING && pid_is_launcher(g_launcher_pid))
		{
			g_launch_state = LAUNCH_RUNNING;
			printf("DVD_MAIN: launcher pid=%ld exec complete\n",
			       (long)g_launcher_pid);
		}
		else if (g_launch_state == LAUNCH_NONE)
			g_launch_state = LAUNCH_STARTING;
		return 1;
	}
	if (r == g_launcher_pid)
	{
		printf("DVD_MAIN: launcher pid=%ld exited status=%d\n",
		       (long)r, st);
		g_owned = 0;
		g_launcher_pid = -1;
		g_launch_state = LAUNCH_NONE;
		return 0;
	}
	if (r < 0)
	{
		printf("DVD_MAIN: waitpid pid=%ld: %s\n",
		       (long)g_launcher_pid, strerror(errno));
		g_owned = 0;
		g_launcher_pid = -1;
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
	if (!owned)
		g_launch_state = LAUNCH_RUNNING;
	else if (g_launch_state == LAUNCH_NONE)
		g_launch_state = LAUNCH_RUNNING;
	printf("DVD_MAIN: using launcher pid=%ld owned=%d state=%d\n",
	       (long)pid, owned, g_launch_state);
}

static int spawn_launcher(int rotate)
{
	pid_t pid;
	const char *old;
	char ld[512];

	if (owned_child_live())
	{
		printf("DVD_MAIN: spawn skipped, owned pid=%ld still live\n",
		       (long)g_launcher_pid);
		return 0;
	}

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

		/* argv[0] must be the full path: launcher derives the player
		 * directory from a slash in argv[0]. Basename-only makes it
		 * look in cwd (typically /) for dvd_av_threaded_test. */
		execl(DVD_LAUNCHER_PATH, DVD_LAUNCHER_PATH, (char *)NULL);
		_exit(127);
	}

	g_last_start_ts = time(NULL);
	g_launch_state = LAUNCH_STARTING;
	adopt_launcher(pid, 1);
	printf("DVD_MAIN: launcher started pid=%ld (starting)\n", (long)pid);
	return 0;
}

static int wait_pid_gone(pid_t pid, int seconds)
{
	int i;

	for (i = 0; i < seconds * 10; i++)
	{
		if (g_owned && pid == g_launcher_pid)
		{
			int st;
			pid_t r = waitpid(pid, &st, WNOHANG);
			if (r == pid)
			{
				g_owned = 0;
				g_launcher_pid = -1;
				g_launch_state = LAUNCH_NONE;
				return 1;
			}
			if (r < 0 && errno != EINTR)
			{
				g_owned = 0;
				g_launcher_pid = -1;
				g_launch_state = LAUNCH_NONE;
				return 1;
			}
		}
		else if (!pid_alive(pid))
			return 1;
		usleep(100000);
	}
	if (g_owned && pid == g_launcher_pid)
	{
		int st;
		if (waitpid(pid, &st, WNOHANG) == pid)
		{
			g_owned = 0;
			g_launcher_pid = -1;
			g_launch_state = LAUNCH_NONE;
			return 1;
		}
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
	g_pending_launch = 0;
	g_launch_not_before = 0;

	pid = g_launcher_pid;
	if (g_owned && pid > 0)
	{
		printf("DVD_MAIN: stopping owned launcher pid=%ld\n", (long)pid);
		kill(pid, SIGTERM);
		if (!wait_pid_gone(pid, TERM_WAIT_SEC))
		{
			printf("DVD_MAIN: SIGKILL launcher pid=%ld\n", (long)pid);
			kill(pid, SIGKILL);
			wait_pid_gone(pid, KILL_WAIT_SEC);
		}
		if (g_owned && g_launcher_pid > 0)
		{
			int st;
			waitpid(g_launcher_pid, &st, WNOHANG);
		}
	}
	else
	{
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
	}

	g_owned = 0;
	g_launcher_pid = -1;
	g_launch_state = LAUNCH_NONE;
	orphan_sweep();
	printf("DVD_MAIN: DVD processes stopped\n");
}

static int launcher_running(void)
{
	if (owned_child_live())
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
		pid_t live = find_live_launcher();
		if (live > 0)
		{
			adopt_launcher(live, 0);
			return;
		}
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
		if (g_session || (g_owned && g_launcher_pid > 0) ||
		    pid_is_launcher(g_launcher_pid) || find_live_launcher() > 0)
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

	/*
	 * Do NOT spawn here. This hook runs from user_io_init() before
	 * parse_config(), video_init() and the core-reset/config pulses.
	 * Launcher DDR activity inside that window stalls the core's DDRAM
	 * mailbox engine (hardware-proven: DVD2 set_seq froze ~110 ms after
	 * FPGA config; the same launcher started late under stock Main runs
	 * indefinitely). Adopt an already-live launcher, otherwise arm a
	 * deferred launch that dvd_main_poll() performs once Main is in its
	 * normal polling loop and the settle delay has elapsed — matching
	 * the old daemon's proven-working timing.
	 */
	if (owned_child_live())
	{
		g_pending_launch = 0;
		return;
	}
	{
		pid_t live = find_live_launcher();
		if (live > 0)
		{
			adopt_launcher(live, 0);
			g_pending_launch = 0;
			return;
		}
	}
	g_pending_launch = 1;
	g_launch_not_before = mono_ms() + LAUNCH_SETTLE_MS;
	printf("DVD_MAIN: launcher start armed (settle %d ms)\n", LAUNCH_SETTLE_MS);
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
	 * Deferred first launch (armed by dvd_main_on_core_ready). Runs only
	 * once Main's init is done and normal polling is active, after the
	 * settle deadline. Leaving DVD-Player or app_restart/reboot teardown
	 * cancels it via dvd_main_stop_all() before we get here. Respawn
	 * accounting starts only after this first real spawn (spawn_launcher
	 * sets g_last_start_ts; log rotation also happens there).
	 *
	 * After fork(), treat the child as STARTING immediately. waitpid is
	 * the liveness test; do not wait for /proc/cmdline to show
	 * dvd_launcher (that lags until execl completes).
	 */
	if (g_pending_launch)
	{
		if (mono_ms() < g_launch_not_before)
			return;
		g_pending_launch = 0;
		printf("DVD_MAIN: settle delay elapsed, starting launcher\n");
		start_or_adopt();
		return;
	}

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
			    (owned_child_live() || pid_is_launcher(g_launcher_pid)))
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
