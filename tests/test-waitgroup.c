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
#include "assert.h"
#include "utils.h"
#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdio.h>


static xylem_opts_t _rt_opts = { .workers = 0 };

#define WG_WORKERS 50

typedef struct {
    xylem_waitgroup_t* wg;
    atomic_int         done_count;
    atomic_int         tested;
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
    xylem_waitgroup_destroy(ctx->wg);
    ctx->wg = NULL;
    atomic_store(&ctx->tested, 1);
}

static void _test_wg_main(void* arg) {
    _wg_ctx_t* ctx = (_wg_ctx_t*)arg;
    ctx->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, WG_WORKERS);
    xylem_spawn(_wg_waiter, ctx);
    for (int i = 0; i < WG_WORKERS; i++) {
        xylem_spawn(_wg_worker, ctx);
    }
}

static void test_concurrent(void) {
    fprintf(stderr, "=== test_concurrent\n");
    for (int round = 0; round < 20; round++) {
        _wg_ctx_t ctx = {0};
        _test_wg_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
    }
}

#define WG_MULTI_WAITERS 16

typedef struct {
    xylem_waitgroup_t* wg;
    atomic_int         done_count;
    atomic_int         waiters_released;
    atomic_int         tested;
} _wg_multi_ctx_t;

static void _wg_multi_worker(void* arg) {
    _wg_multi_ctx_t* ctx = (_wg_multi_ctx_t*)arg;
    atomic_fetch_add(&ctx->done_count, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _wg_multi_waiter(void* arg) {
    _wg_multi_ctx_t* ctx = (_wg_multi_ctx_t*)arg;
    xylem_waitgroup_wait(ctx->wg);
    ASSERT(atomic_load(&ctx->done_count) == WG_WORKERS);
    if (atomic_fetch_add(&ctx->waiters_released, 1) + 1 == WG_MULTI_WAITERS) {
        xylem_waitgroup_destroy(ctx->wg);
        ctx->wg = NULL;
        atomic_store(&ctx->tested, 1);
    }
}

static void _test_wg_multi_main(void* arg) {
    _wg_multi_ctx_t* ctx = (_wg_multi_ctx_t*)arg;
    ctx->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, WG_WORKERS);
    for (int i = 0; i < WG_MULTI_WAITERS; i++) {
        xylem_spawn(_wg_multi_waiter, ctx);
    }
    for (int i = 0; i < WG_WORKERS; i++) {
        xylem_spawn(_wg_multi_worker, ctx);
    }
}

static void test_multi_waiter(void) {
    fprintf(stderr, "=== test_multi_waiter\n");
    for (int round = 0; round < 20; round++) {
        _wg_multi_ctx_t ctx = {0};
        _test_wg_multi_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
        ASSERT(atomic_load(&ctx.waiters_released) == WG_MULTI_WAITERS);
    }
}

#define WGT_WORKERS 8

typedef struct {
    xylem_waitgroup_t* wg;
    atomic_int         done_count;
    atomic_int         thread_released;
    atomic_int         tested;
} _wgt_ctx_t;

static int _wgt_thread_waiter(void* arg) {
    _wgt_ctx_t* ctx = (_wgt_ctx_t*)arg;
    xylem_waitgroup_wait(ctx->wg);
    ASSERT(atomic_load(&ctx->done_count) == WGT_WORKERS);
    atomic_store(&ctx->thread_released, 1);
    return 0;
}

static void _wgt_worker(void* arg) {
    _wgt_ctx_t* ctx = (_wgt_ctx_t*)arg;
    xylem_sleep(5);
    atomic_fetch_add(&ctx->done_count, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _wgt_driver(void* arg) {
    _wgt_ctx_t* ctx = (_wgt_ctx_t*)arg;
    while (atomic_load(&ctx->thread_released) == 0) {
        xylem_sleep(2);
    }
    xylem_waitgroup_destroy(ctx->wg);
    ctx->wg = NULL;
    atomic_store(&ctx->tested, 1);
}

static void _test_wgt_main(void* arg) {
    _wgt_ctx_t* ctx = (_wgt_ctx_t*)arg;
    ctx->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, WGT_WORKERS);
    thrd_t th;
    ASSERT(thrd_create(&th, _wgt_thread_waiter, ctx) == thrd_success);
    thrd_detach(th);
    for (int i = 0; i < WGT_WORKERS; i++) {
        xylem_spawn(_wgt_worker, ctx);
    }
    xylem_spawn(_wgt_driver, ctx);
}

static void test_thread_waiter(void) {
    fprintf(stderr, "=== test_thread_waiter\n");
    for (int round = 0; round < 20; round++) {
        _wgt_ctx_t ctx = {0};
        atomic_init(&ctx.done_count, 0);
        atomic_init(&ctx.thread_released, 0);
        _test_wgt_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
    }
}

#define WGM_CORO_WAITERS   6
#define WGM_THREAD_WAITERS 4
#define WGM_WAITERS        (WGM_CORO_WAITERS + WGM_THREAD_WAITERS)
#define WGM_WORKERS        10

typedef struct {
    xylem_waitgroup_t* wg;
    atomic_int         done_count;
    atomic_int         released;
    atomic_int         tested;
} _wgm_ctx_t;

static int _wgm_thread_waiter(void* arg) {
    _wgm_ctx_t* ctx = (_wgm_ctx_t*)arg;
    xylem_waitgroup_wait(ctx->wg);
    ASSERT(atomic_load(&ctx->done_count) == WGM_WORKERS);
    atomic_fetch_add(&ctx->released, 1);
    return 0;
}

static void _wgm_coro_waiter(void* arg) {
    _wgm_ctx_t* ctx = (_wgm_ctx_t*)arg;
    xylem_waitgroup_wait(ctx->wg);
    ASSERT(atomic_load(&ctx->done_count) == WGM_WORKERS);
    atomic_fetch_add(&ctx->released, 1);
}

static void _wgm_worker(void* arg) {
    _wgm_ctx_t* ctx = (_wgm_ctx_t*)arg;
    xylem_sleep(2);
    atomic_fetch_add(&ctx->done_count, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _wgm_driver(void* arg) {
    _wgm_ctx_t* ctx = (_wgm_ctx_t*)arg;
    while (atomic_load(&ctx->released) < WGM_WAITERS) {
        xylem_sleep(2);
    }
    xylem_waitgroup_destroy(ctx->wg);
    ctx->wg = NULL;
    atomic_store(&ctx->tested, 1);
}

static void _test_wgm_main(void* arg) {
    _wgm_ctx_t* ctx = (_wgm_ctx_t*)arg;
    ctx->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, WGM_WORKERS);
    for (int i = 0; i < WGM_THREAD_WAITERS; i++) {
        thrd_t th;
        ASSERT(thrd_create(&th, _wgm_thread_waiter, ctx) == thrd_success);
        thrd_detach(th);
    }
    for (int i = 0; i < WGM_CORO_WAITERS; i++) {
        xylem_spawn(_wgm_coro_waiter, ctx);
    }
    for (int i = 0; i < WGM_WORKERS; i++) {
        xylem_spawn(_wgm_worker, ctx);
    }
    xylem_spawn(_wgm_driver, ctx);
}

static void test_mixed_waiters(void) {
    fprintf(stderr, "=== test_mixed_waiters\n");
    for (int round = 0; round < 20; round++) {
        _wgm_ctx_t ctx = {0};
        atomic_init(&ctx.done_count, 0);
        atomic_init(&ctx.released, 0);
        _test_wgm_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
        ASSERT(atomic_load(&ctx.released) == WGM_WAITERS);
    }
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_concurrent();
    test_multi_waiter();
    test_thread_waiter();
    test_mixed_waiters();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, &_rt_opts);
    return 0;
}
