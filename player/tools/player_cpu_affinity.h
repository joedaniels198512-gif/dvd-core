/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DVD_PLAYER_CPU_AFFINITY_H
#define DVD_PLAYER_CPU_AFFINITY_H

/* Linux standalone player; define _GNU_SOURCE before system headers. */
#include <errno.h>
#include <sched.h>
#include <unistd.h>

/* MiSTer's launcher may be pinned to one CPU. fork/exec and pthread_create
 * inherit that mask. Restore eligibility on the configured CPUs before
 * creating workers; the kernel still enforces online/cpuset restrictions.
 * This changes only the calling thread, never the launcher or other services. */
static int player_allow_configured_cpus(cpu_set_t *before, cpu_set_t *after)
{
    cpu_set_t requested;
    long count = sysconf(_SC_NPROCESSORS_CONF);

    if (count < 1 || count > CPU_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    if (sched_getaffinity(0, sizeof(*before), before) < 0)
        return -1;
    CPU_ZERO(&requested);
    for (long cpu = 0; cpu < count; cpu++)
        CPU_SET(cpu, &requested);
    if (sched_setaffinity(0, sizeof(requested), &requested) < 0)
        return -1;
    return sched_getaffinity(0, sizeof(*after), after);
}
#endif
