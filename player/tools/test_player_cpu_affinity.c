/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Linux integration test: run as a separate process; never changes its parent.
 * cc -O2 -Wall -Wextra -Werror -pthread test_player_cpu_affinity.c -o test-affinity
 */
#define _GNU_SOURCE
#include "player_cpu_affinity.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static cpu_set_t expected;
static void *worker(void *unused)
{
    cpu_set_t actual;
    (void)unused;
    assert(sched_getaffinity(0, sizeof(actual), &actual) == 0);
    assert(CPU_EQUAL(&actual, &expected));
    return NULL;
}

int main(void)
{
    cpu_set_t initial, before, after, single;
    pthread_t thread;
    assert(sched_getaffinity(0, sizeof(initial), &initial) == 0);
    /* Establish the mask this test process can actually use, including cpusets. */
    assert(player_allow_configured_cpus(&before, &expected) == 0);
    if (CPU_COUNT(&expected) < 2) {
        puts("SKIP: fewer than two eligible CPUs");
        assert(sched_setaffinity(0, sizeof(initial), &initial) == 0);
        return 77;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &expected)) continue;
        CPU_ZERO(&single);
        CPU_SET(cpu, &single);
        assert(sched_setaffinity(0, sizeof(single), &single) == 0);
        assert(player_allow_configured_cpus(&before, &after) == 0);
        assert(CPU_EQUAL(&before, &single));
        assert(CPU_EQUAL(&after, &expected));
        assert(pthread_create(&thread, NULL, worker, NULL) == 0);
        assert(pthread_join(thread, NULL) == 0);
        /* Repeating startup setup is harmless. */
        assert(player_allow_configured_cpus(&before, &after) == 0);
        assert(CPU_EQUAL(&before, &expected) && CPU_EQUAL(&after, &expected));
    }
    assert(sched_setaffinity(0, sizeof(initial), &initial) == 0);
    printf("PASS: restore each single-CPU mask to %d CPUs; worker inheritance; repeat setup\n",
           CPU_COUNT(&expected));
    return 0;
}
