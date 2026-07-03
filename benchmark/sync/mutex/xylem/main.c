/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

#include "xylem.h"

#include "platform/platform-info.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define BENCH_DURATION_MS 5000

typedef enum {
    MODE_CC,
    MODE_TT,
    MODE_CT,
    MODE_TC,
} _mode_t;

static struct {
    _mode_t           mode;
    const char*       mode_name;
    xylem_mutex_t*    mutex;
    xylem_waitgroup_t* wg;
    atomic_int        running;
    int32_t           workers;
    int32_t           tasks;
    uint64_t          counter;
    uint64_t          total_ops;
    uint64_t          elapsed_ns;
} G;

static uint64_t _now_ns(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
}

static int32_t _default_workers(void) {
    int32_t workers = (int32_t)platform_info_getcpus();
    if (workers < 1) {
        workers = 4;
    }
    return workers;
}

static void _worker(void* arg) {
    (void)arg;

    for (;;) {
        xylem_mutex_lock(G.mutex);
        if (!atomic_load(&G.running)) {
            xylem_mutex_unlock(G.mutex);
            break;
        }
        G.counter++;
        xylem_mutex_unlock(G.mutex);
    }
    xylem_waitgroup_done(G.wg);
}

static int _worker_thrd(void* arg) {
    _worker(arg);
    return 0;
}

static void _spawn_thread(void) {
    thrd_t thrd;
    if (thrd_create(&thrd, _worker_thrd, NULL) != thrd_success) {
        abort();
    }
    thrd_detach(thrd);
}

static void _launch_workers(void) {
    int32_t half = G.tasks / 2;

    switch (G.mode) {
    case MODE_TT:
        for (int32_t i = 0; i < G.tasks; i++) {
            _spawn_thread();
        }
        break;
    case MODE_CT:
        for (int32_t i = 0; i < half; i++) {
            xylem_spawn(_worker, NULL);
        }
        for (int32_t i = half; i < G.tasks; i++) {
            _spawn_thread();
        }
        break;
    case MODE_TC:
        for (int32_t i = 0; i < half; i++) {
            _spawn_thread();
        }
        for (int32_t i = half; i < G.tasks; i++) {
            xylem_spawn(_worker, NULL);
        }
        break;
    default:
        for (int32_t i = 0; i < G.tasks; i++) {
            xylem_spawn(_worker, NULL);
        }
        break;
    }
}

static void _run(void* arg) {
    (void)arg;

    G.mutex = xylem_mutex_create();
    G.wg    = xylem_waitgroup_create();
    if (!G.mutex || !G.wg) {
        abort();
    }
    G.counter = 0;

    xylem_waitgroup_add(G.wg, (size_t)G.tasks);

    uint64_t t0 = _now_ns();
    atomic_store(&G.running, 1);

    _launch_workers();

    xylem_sleep((uint64_t)BENCH_DURATION_MS);
    atomic_store(&G.running, 0);

    xylem_waitgroup_wait(G.wg);
    uint64_t t1 = _now_ns();

    G.elapsed_ns = t1 - t0;
    G.total_ops  = G.counter;

    xylem_waitgroup_destroy(G.wg);
    xylem_mutex_destroy(G.mutex);
    G.wg    = NULL;
    G.mutex = NULL;
}

static void _print_result(void) {
    double sec  = (double)G.elapsed_ns / 1e9;
    double ops  = (sec > 0.0) ? (double)G.total_ops / sec : 0.0;
    double nspo = (G.total_ops > 0)
                      ? (double)G.elapsed_ns / (double)G.total_ops
                      : 0.0;

    printf("{\n");
    printf("  \"primitive\": \"mutex\",\n");
    printf("  \"lang\": \"xylem\",\n");
    printf("  \"mode\": \"%s\",\n", G.mode_name);
    printf("  \"workers\": %" PRId32 ",\n", G.workers);
    printf("  \"tasks\": %" PRId32 ",\n", G.tasks);
    printf("  \"duration_ms\": %d,\n", BENCH_DURATION_MS);
    printf("  \"total_ops\": %" PRIu64 ",\n", G.total_ops);
    printf("  \"duration_sec\": %.6f,\n", sec);
    printf("  \"ops_per_sec\": %.0f,\n", ops);
    printf("  \"ns_per_op\": %.2f\n", nspo);
    printf("}\n");
}

int main(void) {
    static const _mode_t modes[] = {MODE_CC, MODE_TT, MODE_CT, MODE_TC};
    static const char*   names[] = {"cc", "tt", "ct", "tc"};

    G.workers = _default_workers();
    G.tasks   = G.workers * 2;
    if (G.tasks < 2) {
        G.tasks = 2;
    }

    xylem_opts_t opts = {.workers = G.workers, .coro_stack_size = 0};
    atomic_init(&G.running, 0);

    for (int i = 0; i < 4; i++) {
        G.mode      = modes[i];
        G.mode_name = names[i];
        xylem_run(_run, NULL, &opts);
        _print_result();
    }
    return 0;
}
