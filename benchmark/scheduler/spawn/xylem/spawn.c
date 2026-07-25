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

#ifndef BENCH_TASKS
#define BENCH_TASKS 1000000
#endif

#if BENCH_TASKS < 1
#error "BENCH_TASKS must be positive"
#endif

typedef struct {
    xylem_waitgroup_t* wg;
    atomic_uint_fast64_t completed;
    uint64_t             elapsed_ns;
} _spawn_ctx_t;

static uint64_t _now_ns(void) {
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC);
}

static void _spawn_task(void* arg) {
    _spawn_ctx_t* ctx = (_spawn_ctx_t*)arg;

    atomic_fetch_add_explicit(&ctx->completed, 1, memory_order_relaxed);
    xylem_waitgroup_done(ctx->wg);
}

static void _run(void* arg) {
    _spawn_ctx_t* ctx = (_spawn_ctx_t*)arg;

    ctx->wg = xylem_waitgroup_create();
    if (!ctx->wg) {
        abort();
    }
    xylem_waitgroup_add(ctx->wg, (size_t)BENCH_TASKS);

    uint64_t started_ns = _now_ns();
    for (uint64_t i = 0; i < (uint64_t)BENCH_TASKS; i++) {
        xylem_spawn(_spawn_task, ctx);
    }
    xylem_waitgroup_wait(ctx->wg);
    ctx->elapsed_ns = _now_ns() - started_ns;

    xylem_waitgroup_destroy(ctx->wg);
    ctx->wg = NULL;
}

static void _print_result(const _spawn_ctx_t* ctx) {
    uint64_t completed = atomic_load_explicit(
        &ctx->completed, memory_order_relaxed);
    double elapsed_sec = (double)ctx->elapsed_ns / 1e9;
    double tasks_per_sec = (double)BENCH_TASKS / elapsed_sec;
    double ns_per_task = (double)ctx->elapsed_ns / (double)BENCH_TASKS;

    printf("{\n");
    printf("  \"benchmark\": \"spawn\",\n");
    printf("  \"lang\": \"xylem\",\n");
    printf("  \"mode\": \"st\",\n");
    printf("  \"workers\": 1,\n");
    printf("  \"tasks\": %" PRIu64 ",\n", (uint64_t)BENCH_TASKS);
    printf("  \"completed\": %" PRIu64 ",\n", completed);
    printf("  \"elapsed_sec\": %.6f,\n", elapsed_sec);
    printf("  \"tasks_per_sec\": %.0f,\n", tasks_per_sec);
    printf("  \"ns_per_task\": %.2f\n", ns_per_task);
    printf("}\n");
}

int main(void) {
    _spawn_ctx_t ctx = {0};
    xylem_opts_t opts = {.workers = 1, .coro_stack_size = 0};

    atomic_init(&ctx.completed, 0);
    xylem_run(_run, &ctx, &opts);
    _print_result(&ctx);

    return atomic_load_explicit(&ctx.completed, memory_order_relaxed) ==
                   (uint64_t)BENCH_TASKS
               ? 0
               : 1;
}
