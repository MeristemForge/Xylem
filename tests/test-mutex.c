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
    atomic_int     tested;
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
        xylem_mutex_destroy(ctx->mtx);
        ctx->mtx = NULL;
        atomic_store(&ctx->tested, 1);
    }
}

static void _test_mtx_main(void* arg) {
    _mtx_ctx_t* ctx = (_mtx_ctx_t*)arg;
    ctx->mtx = xylem_mutex_create();
    for (int i = 0; i < MTX_WORKERS; i++) {
        xylem_spawn(_mtx_worker, ctx);
    }
}

static void test_concurrent(void) {
    fprintf(stderr, "=== test_concurrent\n");
    for (int round = 0; round < 20; round++) {
        _mtx_ctx_t ctx = {0};
        _test_mtx_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
    }
}

typedef struct {
    xylem_mutex_t* mtx;
    atomic_int     tested;
} _mtx_try_ctx_t;

static void _mtx_trylock_coro(void* arg) {
    _mtx_try_ctx_t* ctx = (_mtx_try_ctx_t*)arg;

    ASSERT(xylem_mutex_trylock(ctx->mtx) == true);
    ASSERT(xylem_mutex_trylock(ctx->mtx) == false);
    xylem_mutex_unlock(ctx->mtx);
    ASSERT(xylem_mutex_trylock(ctx->mtx) == true);
    xylem_mutex_unlock(ctx->mtx);

    xylem_mutex_destroy(ctx->mtx);
    ctx->mtx = NULL;
    atomic_store(&ctx->tested, 1);
}

static void _test_mtx_try_main(void* arg) {
    _mtx_try_ctx_t* ctx = (_mtx_try_ctx_t*)arg;
    ctx->mtx = xylem_mutex_create();
    xylem_spawn(_mtx_trylock_coro, ctx);
}

static void test_trylock(void) {
    fprintf(stderr, "=== test_trylock\n");
    _mtx_try_ctx_t ctx = {0};
    _test_mtx_try_main(&ctx);
    while (atomic_load(&ctx.tested) == 0) {
        xylem_sleep(1);
    }
    ASSERT(atomic_load(&ctx.tested) == 1);
}

#define MTX_THREADS        8
#define MTX_THR_INCREMENTS 20000

typedef struct {
    xylem_mutex_t* mtx;
    long long      counter;
    atomic_int     done;
    thrd_t         threads[MTX_THREADS];
} _mtx_thr_ctx_t;
static int _mtx_thr_worker(void* arg) {
    _mtx_thr_ctx_t* ctx = (_mtx_thr_ctx_t*)arg;
    for (int i = 0; i < MTX_THR_INCREMENTS; i++) {
        xylem_mutex_lock(ctx->mtx);
        ctx->counter++;
        xylem_mutex_unlock(ctx->mtx);
    }
    atomic_fetch_add(&ctx->done, 1);
    return 0;
}
static void _test_mtx_thr_main(void* arg) {
    _mtx_thr_ctx_t* ctx = (_mtx_thr_ctx_t*)arg;
    ctx->mtx = xylem_mutex_create();
    for (int i = 0; i < MTX_THREADS; i++) {
        ASSERT(thrd_create(&ctx->threads[i], _mtx_thr_worker, ctx) ==
               thrd_success);
    }
    while (atomic_load(&ctx->done) < MTX_THREADS) {
        xylem_sleep(1);
    }
}

static void test_threads(void) {
    fprintf(stderr, "=== test_threads\n");
    _mtx_thr_ctx_t ctx = {0};
    atomic_init(&ctx.done, 0);

    _thr_wd_t wd = { .timeout_ms = SAFETY_TIMEOUT_MS };
    atomic_init(&wd.stop, 0);
    thrd_t wd_th;
    ASSERT(thrd_create(&wd_th, _thr_wd_fn, &wd) == thrd_success);

    _test_mtx_thr_main(&ctx);

    for (int i = 0; i < MTX_THREADS; i++) {
        thrd_join(ctx.threads[i], NULL);
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
    atomic_int     threads_done;
    thrd_t         threads[MTX_MIX_THREADS];
} _mtx_mixed_ctx_t;
static int _mtx_mixed_thr(void* arg) {
    _mtx_mixed_ctx_t* ctx = (_mtx_mixed_ctx_t*)arg;
    for (int i = 0; i < MTX_MIX_INCREMENTS; i++) {
        xylem_mutex_lock(ctx->mtx);
        ctx->counter++;
        xylem_mutex_unlock(ctx->mtx);
    }
    atomic_fetch_add(&ctx->threads_done, 1);
    return 0;
}
static void _mtx_mixed_coro(void* arg) {
    _mtx_mixed_ctx_t* ctx = (_mtx_mixed_ctx_t*)arg;
    for (int i = 0; i < MTX_MIX_INCREMENTS; i++) {
        xylem_mutex_lock(ctx->mtx);
        ctx->counter++;
        xylem_mutex_unlock(ctx->mtx);
    }
    atomic_fetch_add(&ctx->coros_done, 1);
}
static void _test_mtx_mixed_main(void* arg) {
    _mtx_mixed_ctx_t* ctx = (_mtx_mixed_ctx_t*)arg;
    ctx->mtx = xylem_mutex_create();
    for (int i = 0; i < MTX_MIX_THREADS; i++) {
        ASSERT(thrd_create(&ctx->threads[i], _mtx_mixed_thr, ctx) ==
               thrd_success);
    }
    for (int i = 0; i < MTX_MIX_COROS; i++) {
        xylem_spawn(_mtx_mixed_coro, ctx);
    }
    while (atomic_load(&ctx->coros_done) < MTX_MIX_COROS ||
           atomic_load(&ctx->threads_done) < MTX_MIX_THREADS) {
        xylem_sleep(1);
    }
}

static void test_mixed(void) {
    fprintf(stderr, "=== test_mixed\n");
    _mtx_mixed_ctx_t ctx = {0};
    atomic_init(&ctx.coros_done, 0);
    atomic_init(&ctx.threads_done, 0);

    _thr_wd_t wd = { .timeout_ms = SAFETY_TIMEOUT_MS };
    atomic_init(&wd.stop, 0);
    thrd_t wd_th;
    ASSERT(thrd_create(&wd_th, _thr_wd_fn, &wd) == thrd_success);

    _test_mtx_mixed_main(&ctx);

    for (int i = 0; i < MTX_MIX_THREADS; i++) {
        thrd_join(ctx.threads[i], NULL);
    }

    atomic_store(&wd.stop, 1);
    thrd_join(wd_th, NULL);

    ASSERT(ctx.counter ==
           (long long)(MTX_MIX_COROS + MTX_MIX_THREADS) * MTX_MIX_INCREMENTS);
    xylem_mutex_destroy(ctx.mtx);
}

typedef struct {
    xylem_mutex_t* mtx;
    atomic_int     ready;
    atomic_int     order;
    atomic_int     done;
    int            thread_order;
    int            coro_order;
} _mtx_handoff_ctx_t;

static int _mtx_handoff_thr(void* arg) {
    _mtx_handoff_ctx_t* ctx = (_mtx_handoff_ctx_t*)arg;
    atomic_fetch_add(&ctx->ready, 1);
    xylem_mutex_lock(ctx->mtx);
    ctx->thread_order = atomic_fetch_add(&ctx->order, 1) + 1;
    xylem_mutex_unlock(ctx->mtx);
    atomic_fetch_add(&ctx->done, 1);
    return 0;
}

static void _mtx_handoff_coro(void* arg) {
    _mtx_handoff_ctx_t* ctx = (_mtx_handoff_ctx_t*)arg;
    atomic_fetch_add(&ctx->ready, 1);
    xylem_mutex_lock(ctx->mtx);
    ctx->coro_order = atomic_fetch_add(&ctx->order, 1) + 1;
    xylem_mutex_unlock(ctx->mtx);
    atomic_fetch_add(&ctx->done, 1);
}

static void _test_mtx_handoff_main(void* arg) {
    _mtx_handoff_ctx_t* ctx = (_mtx_handoff_ctx_t*)arg;
    ctx->mtx = xylem_mutex_create();
    xylem_mutex_lock(ctx->mtx);

    thrd_t th;
    ASSERT(thrd_create(&th, _mtx_handoff_thr, ctx) == thrd_success);
    thrd_detach(th);

    while (atomic_load(&ctx->ready) < 1) {
        xylem_sleep(1);
    }
    xylem_sleep(10);

    xylem_spawn(_mtx_handoff_coro, ctx);
    while (atomic_load(&ctx->ready) < 2) {
        xylem_sleep(1);
    }
    xylem_sleep(10);

    xylem_mutex_unlock(ctx->mtx);
    while (atomic_load(&ctx->done) < 2) {
        xylem_sleep(1);
    }

    ASSERT(ctx->thread_order == 1);
    ASSERT(ctx->coro_order == 2);
    xylem_mutex_destroy(ctx->mtx);
    ctx->mtx = NULL;
}

static void test_mixed_handoff_order(void) {
    fprintf(stderr, "=== test_mixed_handoff_order\n");
    for (int round = 0; round < 20; round++) {
        _mtx_handoff_ctx_t ctx = {0};
        atomic_init(&ctx.ready, 0);
        atomic_init(&ctx.order, 0);
        atomic_init(&ctx.done, 0);
        _test_mtx_handoff_main(&ctx);
        ASSERT(ctx.thread_order == 1);
        ASSERT(ctx.coro_order == 2);
    }
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_concurrent();
    test_trylock();
    test_threads();
    test_mixed();
    test_mixed_handoff_order();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, &_rt_opts);
    return 0;
}
