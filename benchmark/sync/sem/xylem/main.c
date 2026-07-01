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
#include "sync/sem.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define BENCH_DURATION_MS 5000

typedef enum {
    MODE_CC,
    MODE_TT,
    MODE_CT,
    MODE_TC,
} _mode_t;

typedef struct {
    void (*fn)(void*);
    void* arg;
} _thrd_pack_t;

static struct {
    _mode_t     mode;
    const char* mode_name;

    sem_t*             sem_a;
    sem_t*             sem_b;
    xylem_waitgroup_t* wg;

    volatile int       running;
    volatile uint64_t  counter;

    uint64_t total_ops;
    uint64_t elapsed_ns;
} G;

static uint64_t _now_ns(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
}

static int _thrd_tramp(void* arg) {
    _thrd_pack_t* p = (_thrd_pack_t*)arg;
    void (*fn)(void*) = p->fn;
    void* fn_arg = p->arg;

    free(p);
    fn(fn_arg);
    return 0;
}

static void _thrd_go(void (*fn)(void*), void* arg) {
    _thrd_pack_t* p = (_thrd_pack_t*)calloc(1, sizeof(*p));

    if (!p) {
        fprintf(stderr, "thrd_go: out of memory\n");
        exit(1);
    }
    p->fn = fn;
    p->arg = arg;
    thrd_t t;
    if (thrd_create(&t, _thrd_tramp, p) != thrd_success) {
        fprintf(stderr, "thrd_go: thrd_create failed\n");
        free(p);
        exit(1);
    }
    thrd_detach(t);
}

static void _producer(void* arg) {
    (void)arg;

    for (;;) {
        sem_wait(G.sem_a);
        sem_post(G.sem_b);
        G.counter++;
        if (!G.running) {
            break;
        }
    }
    xylem_waitgroup_done(G.wg);
}

static void _consumer(void* arg) {
    (void)arg;

    for (;;) {
        sem_wait(G.sem_b);
        sem_post(G.sem_a);
        G.counter++;
        if (!G.running) {
            break;
        }
    }
    xylem_waitgroup_done(G.wg);
}

static void _run(void* arg) {
    (void)arg;

    G.sem_a = sem_create(1);
    G.sem_b = sem_create(0);
    G.wg = xylem_waitgroup_create();
    G.counter = 0;

    xylem_waitgroup_add(G.wg, 2);

    uint64_t t0 = _now_ns();
    G.running = 1;

    switch (G.mode) {
    case MODE_TT:
        _thrd_go(_producer, NULL);
        _thrd_go(_consumer, NULL);
        break;
    case MODE_CT:
        xylem_spawn(_producer, NULL);
        _thrd_go(_consumer, NULL);
        break;
    case MODE_TC:
        _thrd_go(_producer, NULL);
        xylem_spawn(_consumer, NULL);
        break;
    default:
        xylem_spawn(_producer, NULL);
        xylem_spawn(_consumer, NULL);
        break;
    }

    xylem_sleep((uint64_t)BENCH_DURATION_MS);
    G.running = 0;

    sem_post(G.sem_a);
    sem_post(G.sem_b);

    xylem_waitgroup_wait(G.wg);
    uint64_t t1 = _now_ns();

    G.elapsed_ns = t1 - t0;
    G.total_ops = G.counter;

    xylem_waitgroup_destroy(G.wg);
    sem_destroy(G.sem_b);
    sem_destroy(G.sem_a);
}

static void _print_result(void) {
    double sec = (double)G.elapsed_ns / 1e9;
    double ops = (sec > 0.0) ? (double)G.total_ops / sec : 0.0;
    double nspo = (G.total_ops > 0)
                      ? (double)G.elapsed_ns / (double)G.total_ops
                      : 0.0;

    printf("{\n");
    printf("  \"primitive\": \"sem\",\n");
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

    for (int m = 0; m < 4; m++) {
        G.mode = modes[m];
        G.mode_name = names[m];
        xylem_run(_run, NULL, NULL);
        _print_result();
    }
    return 0;
}
