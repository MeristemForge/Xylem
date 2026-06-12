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

#include <stdatomic.h>
#include <stdio.h>

#define SAFETY_TIMEOUT_MS 10000

static xylem_opts_t _rt_opts = { .workers = 4 };

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  cond;
    int            flag;
    atomic_int     finished;
    int            tested;
} _c_one_ctx_t;

static void _c_one_finish(_c_one_ctx_t* ctx) {
    if (atomic_fetch_add(&ctx->finished, 1) == 1) {
        xylem_cond_destroy(ctx->cond);
        ctx->cond = NULL;
        xylem_mutex_destroy(ctx->mtx);
        ctx->mtx = NULL;
        xylem_shutdown();
    }
}

static void _c_one_waiter(void* arg) {
    _c_one_ctx_t* ctx = (_c_one_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    while (!ctx->flag) {
        xylem_cond_wait(ctx->cond, ctx->mtx);
    }
    ctx->tested = 1;
    xylem_mutex_unlock(ctx->mtx);
    _c_one_finish(ctx);
}

static void _c_one_signaler(void* arg) {
    _c_one_ctx_t* ctx = (_c_one_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    ctx->flag = 1;
    xylem_cond_signal(ctx->cond);
    xylem_mutex_unlock(ctx->mtx);
    _c_one_finish(ctx);
}

static void _test_c_one_main(void* arg) {
    _c_one_ctx_t* ctx = (_c_one_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->mtx  = xylem_mutex_create();
    ctx->cond = xylem_cond_create();
    xylem_spawn(_c_one_waiter, ctx);
    xylem_spawn(_c_one_signaler, ctx);
}

static void test_signal_one(void) {
    fprintf(stderr, "=== test_signal_one\n");
    for (int round = 0; round < 20; round++) {
        _c_one_ctx_t ctx = {0};
        xylem_run(_test_c_one_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
    }
}

#define BCAST_WAITERS 32

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  cond;
    xylem_cond_t*  all_parked;
    int            flag;
    int            parked;
    atomic_int     released;
    int            tested;
} _c_bcast_ctx_t;

static void _c_bcast_waiter(void* arg) {
    _c_bcast_ctx_t* ctx = (_c_bcast_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    if (++ctx->parked == BCAST_WAITERS) {
        xylem_cond_signal(ctx->all_parked);
    }
    while (!ctx->flag) {
        xylem_cond_wait(ctx->cond, ctx->mtx);
    }
    xylem_mutex_unlock(ctx->mtx);

    if (atomic_fetch_add(&ctx->released, 1) + 1 == BCAST_WAITERS) {
        ctx->tested = 1;
        xylem_cond_destroy(ctx->all_parked);
        ctx->all_parked = NULL;
        xylem_cond_destroy(ctx->cond);
        ctx->cond = NULL;
        xylem_mutex_destroy(ctx->mtx);
        ctx->mtx = NULL;
        xylem_shutdown();
    }
}

static void _c_bcast_signaler(void* arg) {
    _c_bcast_ctx_t* ctx = (_c_bcast_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    while (ctx->parked < BCAST_WAITERS) {
        xylem_cond_wait(ctx->all_parked, ctx->mtx);
    }
    ctx->flag = 1;
    xylem_cond_broadcast(ctx->cond);
    xylem_mutex_unlock(ctx->mtx);
}

static void _test_c_bcast_main(void* arg) {
    _c_bcast_ctx_t* ctx = (_c_bcast_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->mtx        = xylem_mutex_create();
    ctx->cond       = xylem_cond_create();
    ctx->all_parked = xylem_cond_create();
    for (int i = 0; i < BCAST_WAITERS; i++) {
        xylem_spawn(_c_bcast_waiter, ctx);
    }
    xylem_spawn(_c_bcast_signaler, ctx);
}

static void test_broadcast(void) {
    fprintf(stderr, "=== test_broadcast\n");
    for (int round = 0; round < 10; round++) {
        _c_bcast_ctx_t ctx = {0};
        xylem_run(_test_c_bcast_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
        ASSERT(atomic_load(&ctx.released) == BCAST_WAITERS);
    }
}

#define BQ_CAP       8
#define BQ_PRODUCERS 4
#define BQ_CONSUMERS 4
#define BQ_PER_PROD  500

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  not_empty;
    xylem_cond_t*  not_full;
    int            buf[BQ_CAP];
    int            head;
    int            tail;
    int            count;
    int            closed;
    atomic_int     produced;
    atomic_int     consumed;
    atomic_int     sum;
    int            tested;
} _c_bq_ctx_t;

static void _c_bq_push(_c_bq_ctx_t* ctx, int v) {
    xylem_mutex_lock(ctx->mtx);
    while (ctx->count == BQ_CAP) {
        xylem_cond_wait(ctx->not_full, ctx->mtx);
    }
    ctx->buf[ctx->tail] = v;
    ctx->tail = (ctx->tail + 1) % BQ_CAP;
    ctx->count++;
    xylem_cond_signal(ctx->not_empty);
    xylem_mutex_unlock(ctx->mtx);
}

static int _c_bq_pop(_c_bq_ctx_t* ctx, int* out) {
    xylem_mutex_lock(ctx->mtx);
    while (ctx->count == 0 && !ctx->closed) {
        xylem_cond_wait(ctx->not_empty, ctx->mtx);
    }
    if (ctx->count == 0 && ctx->closed) {
        xylem_mutex_unlock(ctx->mtx);
        return 0;
    }
    *out = ctx->buf[ctx->head];
    ctx->head = (ctx->head + 1) % BQ_CAP;
    ctx->count--;
    xylem_cond_signal(ctx->not_full);
    xylem_mutex_unlock(ctx->mtx);
    return 1;
}

static void _c_bq_producer(void* arg) {
    _c_bq_ctx_t* ctx = (_c_bq_ctx_t*)arg;
    for (int i = 0; i < BQ_PER_PROD; i++) {
        _c_bq_push(ctx, 1);
    }
    if (atomic_fetch_add(&ctx->produced, 1) + 1 == BQ_PRODUCERS) {
        xylem_mutex_lock(ctx->mtx);
        ctx->closed = 1;
        xylem_cond_broadcast(ctx->not_empty);
        xylem_mutex_unlock(ctx->mtx);
    }
}

static void _c_bq_consumer(void* arg) {
    _c_bq_ctx_t* ctx = (_c_bq_ctx_t*)arg;
    int v;
    int local = 0;
    while (_c_bq_pop(ctx, &v)) {
        local += v;
    }
    atomic_fetch_add(&ctx->sum, local);
    if (atomic_fetch_add(&ctx->consumed, 1) + 1 == BQ_CONSUMERS) {
        ASSERT(atomic_load(&ctx->sum) == BQ_PRODUCERS * BQ_PER_PROD);
        ctx->tested = 1;
        xylem_cond_destroy(ctx->not_full);
        ctx->not_full = NULL;
        xylem_cond_destroy(ctx->not_empty);
        ctx->not_empty = NULL;
        xylem_mutex_destroy(ctx->mtx);
        ctx->mtx = NULL;
        xylem_shutdown();
    }
}

static void _test_c_bq_main(void* arg) {
    _c_bq_ctx_t* ctx = (_c_bq_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->mtx       = xylem_mutex_create();
    ctx->not_empty = xylem_cond_create();
    ctx->not_full  = xylem_cond_create();
    for (int i = 0; i < BQ_CONSUMERS; i++) {
        xylem_spawn(_c_bq_consumer, ctx);
    }
    for (int i = 0; i < BQ_PRODUCERS; i++) {
        xylem_spawn(_c_bq_producer, ctx);
    }
}

static void test_bounded_queue(void) {
    fprintf(stderr, "=== test_bounded_queue\n");
    for (int round = 0; round < 5; round++) {
        _c_bq_ctx_t ctx = {0};
        xylem_run(_test_c_bq_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
    }
}

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  cond;
    xylem_cond_t*  parked_cond;
    int            parked;
    atomic_int     ready;
    int            tested;
} _c_ext_ctx_t;

static void _c_ext_external(void* arg) {
    _c_ext_ctx_t* ctx = (_c_ext_ctx_t*)arg;
    atomic_store(&ctx->ready, 1);
}

static void _c_ext_waiter(void* arg) {
    _c_ext_ctx_t* ctx = (_c_ext_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    ctx->parked = 1;
    xylem_cond_signal(ctx->parked_cond);
    while (atomic_load(&ctx->ready) == 0) {
        xylem_cond_wait(ctx->cond, ctx->mtx);
    }
    xylem_mutex_unlock(ctx->mtx);
    ctx->tested = 1;
    xylem_cond_destroy(ctx->parked_cond);
    ctx->parked_cond = NULL;
    xylem_cond_destroy(ctx->cond);
    ctx->cond = NULL;
    xylem_mutex_destroy(ctx->mtx);
    ctx->mtx = NULL;
    xylem_shutdown();
}

static void _c_ext_submitter(void* arg) {
    _c_ext_ctx_t* ctx = (_c_ext_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    while (!ctx->parked) {
        xylem_cond_wait(ctx->parked_cond, ctx->mtx);
    }
    xylem_mutex_unlock(ctx->mtx);
    int rc = xylem_await(_c_ext_external, ctx);
    ASSERT(rc == 0);
    xylem_cond_broadcast(ctx->cond);
}

static void _test_c_ext_main(void* arg) {
    _c_ext_ctx_t* ctx = (_c_ext_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->mtx         = xylem_mutex_create();
    ctx->cond        = xylem_cond_create();
    ctx->parked_cond = xylem_cond_create();
    xylem_spawn(_c_ext_waiter, ctx);
    xylem_spawn(_c_ext_submitter, ctx);
}

static void test_external_signal(void) {
    fprintf(stderr, "=== test_external_signal\n");
    for (int round = 0; round < 10; round++) {
        _c_ext_ctx_t ctx = {0};
        xylem_run(_test_c_ext_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
    }
}

int main(void) {
    test_signal_one();
    test_broadcast();
    test_bounded_queue();
    test_external_signal();
    return 0;
}
