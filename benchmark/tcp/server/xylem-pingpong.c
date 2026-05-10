/**
 * Pure coroutine-switch microbenchmark.
 *
 * N producer/consumer pairs exchange void* messages over unbounded
 * channels for DURATION seconds. No network, no syscalls on the hot
 * path. Measures the lower bound on coroutine switch + channel wake
 * throughput, isolating the scheduler from net-poll overhead.
 *
 * Usage: xylem-pingpong <pairs> <duration_sec> [workers]
 *
 * Output line: "pingpong pairs=%d dur=%ds workers=%d msgs=%lu rate=%lu"
 */

#include "xylem.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    xylem_channel_t* tx;
    xylem_channel_t* rx;
    _Atomic uint64_t sent;
    _Atomic uint64_t recvd;
    _Atomic int      stop;
} _pair_t;

static void _producer(void* arg) {
    _pair_t* p = (_pair_t*)arg;
    uint64_t n = 0;
    while (!atomic_load_explicit(&p->stop, memory_order_relaxed)) {
        if (xylem_channel_send(p->tx, (void*)(uintptr_t)(n + 1)) != 0) {
            break;
        }
        if (!xylem_channel_recv(p->rx)) {
            break;
        }
        n++;
    }
    atomic_fetch_add_explicit(&p->sent, n, memory_order_relaxed);
}

static void _consumer(void* arg) {
    _pair_t* p = (_pair_t*)arg;
    uint64_t n = 0;
    while (!atomic_load_explicit(&p->stop, memory_order_relaxed)) {
        void* m = xylem_channel_recv(p->tx);
        if (!m) {
            break;
        }
        if (xylem_channel_send(p->rx, m) != 0) {
            break;
        }
        n++;
    }
    atomic_fetch_add_explicit(&p->recvd, n, memory_order_relaxed);
}

static int g_pairs;
static int g_duration_sec;

static void _main(void* arg) {
    (void)arg;
    _pair_t* pairs = (_pair_t*)calloc((size_t)g_pairs, sizeof(_pair_t));
    if (!pairs) {
        xylem_shutdown();
        return;
    }

    for (int i = 0; i < g_pairs; i++) {
        pairs[i].tx = xylem_channel_create();
        pairs[i].rx = xylem_channel_create();
        xylem_spawn(_producer, &pairs[i]);
        xylem_spawn(_consumer, &pairs[i]);
    }

    /* Let coroutines run for DURATION seconds. xylem has no sleep on
     * the main coroutine API here, so block the spawning coro on a
     * timer via a channel with a goroutine. Simpler: busy-spin check
     * using clock_gettime. We run inside a coroutine, so just yield
     * repeatedly. */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        xylem_sleep(100);
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        double elapsed = (double)(t.tv_sec - t0.tv_sec) +
                         (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
        if (elapsed >= (double)g_duration_sec) {
            break;
        }
    }

    for (int i = 0; i < g_pairs; i++) {
        atomic_store_explicit(&pairs[i].stop, 1, memory_order_relaxed);
    }

    /* Drain any remaining messages so recv wakes and exits. Sending a
     * sentinel NULL is not supported (NULL = destroyed). Simpler:
     * rely on channel_destroy to wake them. */
    for (int i = 0; i < g_pairs; i++) {
        xylem_channel_destroy(pairs[i].tx);
        xylem_channel_destroy(pairs[i].rx);
    }

    xylem_sleep(50);

    uint64_t total = 0;
    for (int i = 0; i < g_pairs; i++) {
        total += atomic_load_explicit(&pairs[i].sent, memory_order_relaxed);
    }
    uint64_t rate = total / (uint64_t)g_duration_sec;
    fprintf(stdout,
            "pingpong pairs=%d dur=%ds msgs=%llu rate=%llu\n",
            g_pairs,
            g_duration_sec,
            (unsigned long long)total,
            (unsigned long long)rate);

    free(pairs);
    xylem_shutdown();
}

int main(int argc, char** argv) {
    g_pairs        = (argc > 1) ? atoi(argv[1]) : 100;
    g_duration_sec = (argc > 2) ? atoi(argv[2]) : 5;
    int workers    = (argc > 3) ? atoi(argv[3]) : 0;

    xylem_opts_t opts = {0};
    opts.workers      = workers;

    xylem_run(_main, NULL, &opts);
    return 0;
}
