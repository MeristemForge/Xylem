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
    _mode_t            mode;
    const char*        mode_name;
    xylem_cond_t*      cond;
    xylem_mutex_t*     mutex;
    xylem_waitgroup_t* wg;
    atomic_int         running;
    int32_t            turn;
    uint64_t           counter;
    uint64_t           total_ops;
    uint64_t           elapsed_ns;
} G;

static uint64_t _now_ns(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
}

static void _peer(void* arg) {
    int32_t me    = (int32_t)(intptr_t)arg;
    int32_t other = 1 - me;

    for (;;) {
        xylem_mutex_lock(G.mutex);
        while (G.turn != me && atomic_load(&G.running)) {
            xylem_cond_wait(G.cond, G.mutex);
        }
        if (!atomic_load(&G.running)) {
            G.turn = other;
            xylem_cond_signal(G.cond);
            xylem_mutex_unlock(G.mutex);
            break;
        }
        G.counter++;
        G.turn = other;
        xylem_cond_signal(G.cond);
        xylem_mutex_unlock(G.mutex);
    }
    xylem_waitgroup_done(G.wg);
}

static int _peer_thrd(void* arg) {
    _peer(arg);
    return 0;
}

static void _spawn_thread(int32_t peer) {
    thrd_t thrd;
    if (thrd_create(&thrd, _peer_thrd, (void*)(intptr_t)peer) !=
        thrd_success) {
        abort();
    }
    thrd_detach(thrd);
}

static void _launch_peers(void) {
    switch (G.mode) {
    case MODE_TT:
        _spawn_thread(0);
        _spawn_thread(1);
        break;
    case MODE_CT:
        xylem_spawn(_peer, (void*)(intptr_t)0);
        _spawn_thread(1);
        break;
    case MODE_TC:
        _spawn_thread(0);
        xylem_spawn(_peer, (void*)(intptr_t)1);
        break;
    default:
        xylem_spawn(_peer, (void*)(intptr_t)0);
        xylem_spawn(_peer, (void*)(intptr_t)1);
        break;
    }
}

static void _run(void* arg) {
    (void)arg;

    G.cond  = xylem_cond_create();
    G.mutex = xylem_mutex_create();
    G.wg    = xylem_waitgroup_create();
    if (!G.cond || !G.mutex || !G.wg) {
        abort();
    }
    G.counter = 0;
    G.turn    = 0;

    xylem_waitgroup_add(G.wg, 2);

    uint64_t t0 = _now_ns();
    atomic_store(&G.running, 1);

    _launch_peers();

    xylem_sleep((uint64_t)BENCH_DURATION_MS);

    xylem_mutex_lock(G.mutex);
    atomic_store(&G.running, 0);
    xylem_cond_broadcast(G.cond);
    xylem_mutex_unlock(G.mutex);

    xylem_waitgroup_wait(G.wg);
    uint64_t t1 = _now_ns();

    G.elapsed_ns = t1 - t0;
    G.total_ops  = G.counter;

    xylem_waitgroup_destroy(G.wg);
    xylem_mutex_destroy(G.mutex);
    xylem_cond_destroy(G.cond);
    G.wg    = NULL;
    G.mutex = NULL;
    G.cond  = NULL;
}

static void _print_result(void) {
    double sec  = (double)G.elapsed_ns / 1e9;
    double ops  = (sec > 0.0) ? (double)G.total_ops / sec : 0.0;
    double nspo = (G.total_ops > 0)
                      ? (double)G.elapsed_ns / (double)G.total_ops
                      : 0.0;

    printf("{\n");
    printf("  \"primitive\": \"cond\",\n");
    printf("  \"lang\": \"xylem\",\n");
    printf("  \"mode\": \"%s\",\n", G.mode_name);
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

    atomic_init(&G.running, 0);

    for (int32_t i = 0; i < 4; i++) {
        G.mode      = modes[i];
        G.mode_name = names[i];
        xylem_run(_run, NULL, NULL);
        _print_result();
    }
    return 0;
}
