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
    xylem_channel_t*   ch;
    xylem_waitgroup_t* send_wg;
    xylem_waitgroup_t* recv_wg;
    atomic_int         running;
    _Atomic uint64_t   counter;
    uint64_t           payload;
    uint64_t           total_ops;
    uint64_t           elapsed_ns;
} G;

static uint64_t _now_ns(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
}

static void _sender(void* arg) {
    (void)arg;

    while (atomic_load(&G.running)) {
        if (xylem_channel_send(G.ch, &G.payload) != 0) {
            abort();
        }
    }
    xylem_waitgroup_done(G.send_wg);
}

static void _receiver(void* arg) {
    (void)arg;

    for (;;) {
        void* msg = xylem_channel_recv(G.ch);
        if (!msg) {
            break;
        }
        if (!atomic_load(&G.running)) {
            break;
        }
        atomic_fetch_add(&G.counter, 1);
    }
    xylem_waitgroup_done(G.recv_wg);
}

static int _sender_thrd(void* arg) {
    _sender(arg);
    return 0;
}

static int _receiver_thrd(void* arg) {
    _receiver(arg);
    return 0;
}

static void _spawn_sender_thread(void) {
    thrd_t thrd;
    if (thrd_create(&thrd, _sender_thrd, NULL) != thrd_success) {
        abort();
    }
    thrd_detach(thrd);
}

static void _spawn_receiver_thread(void) {
    thrd_t thrd;
    if (thrd_create(&thrd, _receiver_thrd, NULL) != thrd_success) {
        abort();
    }
    thrd_detach(thrd);
}

static void _launch_pair(void) {
    switch (G.mode) {
    case MODE_TT:
        _spawn_sender_thread();
        _spawn_receiver_thread();
        break;
    case MODE_CT:
        xylem_spawn(_sender, NULL);
        _spawn_receiver_thread();
        break;
    case MODE_TC:
        _spawn_sender_thread();
        xylem_spawn(_receiver, NULL);
        break;
    default:
        xylem_spawn(_sender, NULL);
        xylem_spawn(_receiver, NULL);
        break;
    }
}

static void _run(void* arg) {
    (void)arg;

    G.ch      = xylem_channel_create();
    G.send_wg = xylem_waitgroup_create();
    G.recv_wg = xylem_waitgroup_create();
    if (!G.ch || !G.send_wg || !G.recv_wg) {
        abort();
    }
    atomic_store(&G.counter, 0);
    G.payload = 1;

    xylem_waitgroup_add(G.send_wg, 1);
    xylem_waitgroup_add(G.recv_wg, 1);

    uint64_t t0 = _now_ns();
    atomic_store(&G.running, 1);

    _launch_pair();

    xylem_sleep((uint64_t)BENCH_DURATION_MS);
    uint64_t t1 = _now_ns();
    atomic_store(&G.running, 0);

    G.elapsed_ns = t1 - t0;
    G.total_ops  = atomic_load(&G.counter);

    xylem_waitgroup_wait(G.send_wg);
    xylem_channel_close(G.ch);
    xylem_waitgroup_wait(G.recv_wg);

    xylem_waitgroup_destroy(G.recv_wg);
    xylem_waitgroup_destroy(G.send_wg);
    xylem_channel_destroy(G.ch);
    G.recv_wg = NULL;
    G.send_wg = NULL;
    G.ch      = NULL;
}

static void _print_result(void) {
    double sec  = (double)G.elapsed_ns / 1e9;
    double ops  = (sec > 0.0) ? (double)G.total_ops / sec : 0.0;
    double nspo = (G.total_ops > 0)
                      ? (double)G.elapsed_ns / (double)G.total_ops
                      : 0.0;

    printf("{\n");
    printf("  \"primitive\": \"channel\",\n");
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
    atomic_init(&G.counter, 0);

    for (int32_t i = 0; i < 4; i++) {
        G.mode      = modes[i];
        G.mode_name = names[i];
        xylem_run(_run, NULL, NULL);
        _print_result();
    }
    return 0;
}
