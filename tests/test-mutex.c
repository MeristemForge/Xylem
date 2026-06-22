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

#define SAFETY_TIMEOUT_MS 10000

static xylem_opts_t _rt_opts = { .workers = 0 };

typedef struct {
    atomic_int stop;
    uint64_t   timeout_ms;
} _thr_wd_t;

static int _thr_wd_fn(void* arg) {
    _thr_wd_t* wd = (_thr_wd_t*)arg;
    uint64_t waited = 0;
    while (atomic_load(&wd->stop) == 0) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        thrd_sleep(&ts, NULL);
        waited += 10;
        if (waited >= wd->timeout_ms) {
            fprintf(stderr, "thread-section watchdog: timed out\n");
            abort();
        }
    }
    return 0;
}

#define MTX_WORKERS    20
#define MTX_INCREMENTS 100

typedef struct {
    xylem_mutex_t* mtx;
    int            counter;
    atomic_int     finished;
    int            tested;
} _mtx_ctx_t;

static void _mtx_worker(void* arg) {
    _mtx_ctx_t* ctx = (_mtx_ctx_t*)arg;
    for (int i = 0; i < MTX_INCREMENTS; i++) {
        xylem_mutex_lock(ctx->mtx);
        ctx->counter++;
        xylem_mutex_unlock(ctx->mtx);
    }
    if (atomic_fetch_add(&ctx->finished, 1) == MTX_WORKERS - 1) {
        ASSERT(ctx->counter == MTX_WORKERS * MTX_INCREMENTS);
        ctx->tested = 1;
        xylem_mutex_destroy(ctx->mtx);
        ctx->mtx = NULL;
        xylem_shutdown();
    }
}

static void _test_mtx_main(void* arg) {
    _mtx_ctx_t* ctx = (_mtx_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->mtx = xylem_mutex_create();
    for (int i = 0; i < MTX_WORKERS; i++) {
        xylem_spawn(_mtx_worker, ctx);
    }
}

static void test_concurrent(void) {
    fprintf(stderr, "=== test_concurrent\n");
    for (int round = 0; round < 20; round++) {
        _mtx_ctx_t ctx = {0};
        xylem_run(_test_mtx_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
    }
}

typedef struct {
    xylem_mutex_t* mtx;
    int            tested;
} _mtx_try_ctx_t;

static void _mtx_trylock_coro(void* arg) {
    _mtx_try_ctx_t* ctx = (_mtx_try_ctx_t*)arg;

    ASSERT(xylem_mutex_trylock(ctx->mtx) == true);
    ASSERT(xylem_mutex_trylock(ctx->mtx) == false);
    xylem_mutex_unlock(ctx->mtx);
    ASSERT(xylem_mutex_trylock(ctx->mtx) == true);
    xylem_mutex_unlock(ctx->mtx);

    ctx->tested = 1;
    xylem_mutex_destroy(ctx->mtx);
    ctx->mtx = NULL;
    xylem_shutdown();
}

static void _test_mtx_try_main(void* arg) {
    _mtx_try_ctx_t* ctx = (_mtx_try_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->mtx = xylem_mutex_create();
    xylem_spawn(_mtx_trylock_coro, ctx);
}

static void test_trylock(void) {
    fprintf(stderr, "=== test_trylock\n");
    _mtx_try_ctx_t ctx = {0};
    xylem_run(_test_mtx_try_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

#define MTX_THREADS        8
#define MTX_THR_INCREMENTS 20000

typedef struct {
    xylem_mutex_t* mtx;
    long long      counter;
} _mtx_thr_ctx_t;

static int _mtx_thr_worker(void* arg) {
    _mtx_thr_ctx_t* ctx = (_mtx_thr_ctx_t*)arg;
    for (int i = 0; i < MTX_THR_INCREMENTS; i++) {
        xylem_mutex_lock(ctx->mtx);
        ctx->counter++;
        xylem_mutex_unlock(ctx->mtx);
    }
    return 0;
}

static void test_threads(void) {
    fprintf(stderr, "=== test_threads\n");
    _mtx_thr_ctx_t ctx = {0};
    ctx.mtx = xylem_mutex_create();

    _thr_wd_t wd = { .timeout_ms = SAFETY_TIMEOUT_MS };
    atomic_init(&wd.stop, 0);
    thrd_t wd_th;
    ASSERT(thrd_create(&wd_th, _thr_wd_fn, &wd) == thrd_success);

    thrd_t th[MTX_THREADS];
    for (int i = 0; i < MTX_THREADS; i++) {
        ASSERT(thrd_create(&th[i], _mtx_thr_worker, &ctx) == thrd_success);
    }
    for (int i = 0; i < MTX_THREADS; i++) {
        thrd_join(th[i], NULL);
    }

    atomic_store(&wd.stop, 1);
    thrd_join(wd_th, NULL);

    ASSERT(ctx.counter == (long long)MTX_THREADS * MTX_THR_INCREMENTS);
    xylem_mutex_destroy(ctx.mtx);
}

#define MTX_MIX_COROS      8
#define MTX_MIX_THREADS    8
#define MTX_MIX_INCREMENTS 5000

typedef struct {
    xylem_mutex_t* mtx;
    long long      counter;
    atomic_int     coros_done;
} _mtx_mixed_ctx_t;

static int _mtx_mixed_thr(void* arg) {
    _mtx_mixed_ctx_t* ctx = (_mtx_mixed_ctx_t*)arg;
    for (int i = 0; i < MTX_MIX_INCREMENTS; i++) {
        xylem_mutex_lock(ctx->mtx);
        ctx->counter++;
        xylem_mutex_unlock(ctx->mtx);
    }
    return 0;
}

static void _mtx_mixed_coro(void* arg) {
    _mtx_mixed_ctx_t* ctx = (_mtx_mixed_ctx_t*)arg;
    for (int i = 0; i < MTX_MIX_INCREMENTS; i++) {
        xylem_mutex_lock(ctx->mtx);
        ctx->counter++;
        xylem_mutex_unlock(ctx->mtx);
    }
    if (atomic_fetch_add(&ctx->coros_done, 1) == MTX_MIX_COROS - 1) {
        xylem_shutdown();
    }
}

static void _test_mtx_mixed_main(void* arg) {
    _mtx_mixed_ctx_t* ctx = (_mtx_mixed_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    for (int i = 0; i < MTX_MIX_COROS; i++) {
        xylem_spawn(_mtx_mixed_coro, ctx);
    }
}

static void test_mixed(void) {
    fprintf(stderr, "=== test_mixed\n");
    _mtx_mixed_ctx_t ctx = {0};
    ctx.mtx = xylem_mutex_create();

    _thr_wd_t wd = { .timeout_ms = SAFETY_TIMEOUT_MS };
    atomic_init(&wd.stop, 0);
    thrd_t wd_th;
    ASSERT(thrd_create(&wd_th, _thr_wd_fn, &wd) == thrd_success);

    thrd_t th[MTX_MIX_THREADS];
    for (int i = 0; i < MTX_MIX_THREADS; i++) {
        ASSERT(thrd_create(&th[i], _mtx_mixed_thr, &ctx) == thrd_success);
    }

    xylem_run(_test_mtx_mixed_main, &ctx, &_rt_opts);

    for (int i = 0; i < MTX_MIX_THREADS; i++) {
        thrd_join(th[i], NULL);
    }

    atomic_store(&wd.stop, 1);
    thrd_join(wd_th, NULL);

    ASSERT(ctx.counter ==
           (long long)(MTX_MIX_COROS + MTX_MIX_THREADS) * MTX_MIX_INCREMENTS);
    xylem_mutex_destroy(ctx.mtx);
}

int main(void) {
    test_concurrent();
    test_trylock();
    test_threads();
    test_mixed();
    return 0;
}
