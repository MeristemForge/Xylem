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
#include "runtime/runtime.h"
#include "runtime/sched-timer.h"
#include "assert.h"

#include <stdatomic.h>
#include <stdio.h>

#define SAFETY_TIMEOUT_MS 5000

static xylem_runtime_opts_t _rt_opts = { .workers = 4 };

static void _safety_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)ud;
    sched_timer_destroy(timer);
    xylem_runtime_stop();
    ASSERT(0 && "test timed out");
}

static void _start_safety_timer(void) {
    sched_timer_mgr_t* mgr =
        scheduler_get_timer_mgr(runtime_get_scheduler());
    sched_timer_t* t = sched_timer_create(mgr);
    sched_timer_start(t, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);
}

#define WG_WORKERS 50

typedef struct {
    xylem_waitgroup_t* wg;
    atomic_int         done_count;
    int                tested;
} _wg_ctx_t;

static void _wg_worker(void* arg) {
    _wg_ctx_t* ctx = (_wg_ctx_t*)arg;
    atomic_fetch_add(&ctx->done_count, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _wg_waiter(void* arg) {
    _wg_ctx_t* ctx = (_wg_ctx_t*)arg;
    xylem_waitgroup_wait(ctx->wg);
    ASSERT(atomic_load(&ctx->done_count) == WG_WORKERS);
    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_wg_main(void* arg) {
    _wg_ctx_t* ctx = (_wg_ctx_t*)arg;
    _start_safety_timer();
    ctx->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, WG_WORKERS);
    xylem_runtime_spawn(_wg_waiter, ctx);
    for (int i = 0; i < WG_WORKERS; i++) {
        xylem_runtime_spawn(_wg_worker, ctx);
    }
}

static void test_waitgroup_concurrent(void) {
    fprintf(stderr, "=== test_waitgroup_concurrent\n");
    for (int round = 0; round < 20; round++) {
        _wg_ctx_t ctx = {0};
        xylem_runtime_start(_test_wg_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
        xylem_waitgroup_destroy(ctx.wg);
    }
}

int main(void) {
    test_waitgroup_concurrent();
    return 0;
}
