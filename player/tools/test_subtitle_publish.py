#!/usr/bin/env python3
"""Exercise production subtitle publication on the host without FFmpeg/FPGA.

Extract the actual queue helpers and types, and substitute only the player shell,
clock and diagnostic snapshot. Test storage ownership, queue pressure, reset
invalidation and concurrent presentation. Does not emulate DVD or FPGA timing.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

source = Path(__file__).with_name("dvd_av_threaded_test.c").read_text()


def function(name):
    # Select the definition, never a forward declaration.
    match = re.search(r"\nstatic [^\n]+\b" + name + r"\([^;]+?\n\{", source)
    assert match, name
    start = match.start()
    end = source.index("\n}\n", match.end()) + 3
    return source[start:end]


types = source[source.index("#define MSUB_EVT_MAX"):source.index("} MsubDecoded;") + 14]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#define AV_NOPTS_VALUE INT64_MIN
'''
shell = r'''
typedef struct {
    struct {
        pthread_mutex_t mu;
        MsubDecoded plane[MSUB_Q_CAP], staging;
        unsigned decode_epoch;
        int cur, last_slot;
        unsigned long q_drop;
    } msub;
    int64_t clock;
    unsigned nav_gen, snapshot_id;
} Player;
static unsigned player_nav_gen(Player *p) { return p->nav_gen; }
static int64_t clock_read(int64_t *clock, void *a, void *b)
{ (void)a; (void)b; return *clock; }
static int64_t av_gettime_relative(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void movie_sub_snapshot_decoded(Player *p, const MsubDecoded *s)
{ p->snapshot_id = s->id; }
'''
helpers = "\n".join(function(name) for name in [
    "movie_sub_q_depth", "movie_sub_slot_free_index", "movie_sub_slot_expired",
    "movie_sub_slot_release", "movie_sub_publish_decoded"])
tests = r'''
static Player p;
static int publish(unsigned epoch, unsigned nav)
{
    int depth = -1;
    int64_t aclk, wait, hold;
    int r = movie_sub_publish_decoded(&p, &p.msub.staging, epoch, nav,
                                     &aclk, &depth, &wait, &hold);
    assert(aclk == p.clock && wait >= 0 && hold >= 0);
    if (r > 0) assert(depth == movie_sub_q_depth(&p));
    assert(!pthread_mutex_trylock(&p.msub.mu));
    pthread_mutex_unlock(&p.msub.mu);
    return r;
}
static void check_ownership(void)
{
    for (int i = 0; i <= MSUB_Q_CAP; i++) {
        MsubDecoded *a = i == MSUB_Q_CAP ? &p.msub.staging : &p.msub.plane[i];
        for (int j = 0; j < i; j++) {
            assert(a->idx != p.msub.plane[j].idx);
            assert(a->runs != p.msub.plane[j].runs);
        }
    }
}
static atomic_int started, finished;
static void *publisher(void *unused)
{
    (void)unused;
    atomic_store(&started, 1);
    assert(publish(p.msub.decode_epoch, p.nav_gen) == 1);
    atomic_store(&finished, 1);
    return NULL;
}
int main(void)
{
    pthread_mutex_init(&p.msub.mu, NULL);
    p.msub.cur = -1;
    p.msub.last_slot = -1;
    p.clock = 100;
    p.nav_gen = 5;
    p.msub.decode_epoch = 3;
    for (int i = 0; i <= MSUB_Q_CAP; i++) {
        MsubDecoded *s = i == MSUB_Q_CAP ? &p.msub.staging : &p.msub.plane[i];
        s->idx = calloc(1, SPU_IDX_MAX);
        s->runs = calloc(MSUB_RUN_MAX, sizeof(MsubRun));
        assert(s->idx && s->runs);
    }
    MsubDecoded *s = &p.msub.staging;
    s->id = 42; s->valid = 1; s->until_us = 200;
    s->idx[0] = 3; s->runs[0].code = 3;
    uint8_t *decoded = s->idx, *spare = p.msub.plane[0].idx;
    assert(publish(3, 5) == 1);
    assert(p.msub.plane[0].idx == decoded && s->idx == spare);
    assert(p.msub.last_slot == 0 && p.snapshot_id == 42);
    p.msub.cur = 0;
    memset(s->idx, 1, SPU_IDX_MAX); /* next decode cannot overwrite current */
    assert(p.msub.plane[0].idx[0] == 3);
    check_ownership();

    assert(publish(2, 5) == 0); /* reset / stream epoch changed */
    assert(publish(3, 4) == 0); /* navigation changed */
    assert(movie_sub_q_depth(&p) == 1 && p.msub.q_drop == 0);

    for (int i = 1; i < MSUB_Q_CAP; i++) {
        p.msub.plane[i].occupied = p.msub.plane[i].valid = 1;
        p.msub.plane[i].until_us = 200;
    }
    assert(publish(3, 5) == -1 && p.msub.q_drop == 1);
    assert(s->idx == spare && p.msub.plane[0].idx == decoded);
    p.msub.plane[0].until_us = 50; /* even expired current remains untouched */
    assert(publish(3, 5) == -1);
    p.msub.plane[2].until_us = 50;
    assert(publish(3, 5) == 1 && p.msub.last_slot == 2);
    assert(p.msub.plane[0].idx == decoded && p.msub.plane[0].idx[0] == 3);
    check_ownership();

    /* A presenter holding the lock retains its plane until publication can run. */
    movie_sub_slot_release(&p.msub.plane[1]);
    pthread_mutex_lock(&p.msub.mu);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, publisher, NULL));
    while (!atomic_load(&started)) { }
    assert(!atomic_load(&finished));
    assert(p.msub.plane[0].idx[0] == 3);
    pthread_mutex_unlock(&p.msub.mu);
    pthread_join(thread, NULL);
    assert(atomic_load(&finished));
    check_ownership();

    for (int n = 0; n < 1000; n++) {
        movie_sub_slot_release(&p.msub.plane[1]);
        s->idx[0] = n & 3;
        assert(publish(3, 5) == 1);
        assert(p.msub.plane[1].idx[0] == (n & 3));
        check_ownership();
    }
    for (int i = 0; i <= MSUB_Q_CAP; i++) {
        MsubDecoded *a = i == MSUB_Q_CAP ? s : &p.msub.plane[i];
        free(a->idx); free(a->runs);
    }
    pthread_mutex_destroy(&p.msub.mu);
    puts("PASS: production publication; ownership, full/expired queue, reset/nav invalidation, concurrent presenter, 1000 reuse cycles");
}
'''
with tempfile.TemporaryDirectory(prefix="dvd-subtitle-publish-") as temp:
    path = Path(temp)
    (path / "test.c").write_text(prefix + types + shell + helpers + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-Wall",
                    "-Wextra", "-Werror", "-fsanitize=address,undefined", "-pthread",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
