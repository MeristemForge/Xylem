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

#define CREDIT_WAKE_COUNT 129
#define TIMED_BROADCAST_WAITERS 1024

static xylem_opts_t _functional_rt_opts = { .workers = 4 };
static xylem_opts_t _credit_rt_opts     = { .workers = 1 };

static void test_timedwait_zero(void) {
    fprintf(stderr, "=== test_timedwait_zero\n");

    xylem_mutex_t* mtx  = xylem_mutex_create();
    xylem_cond_t*  cond = xylem_cond_create();
    ASSERT(mtx != NULL);
    ASSERT(cond != NULL);

    xylem_mutex_lock(mtx);
    ASSERT(xylem_cond_timedwait(cond, mtx, 0) == false);
    ASSERT(xylem_mutex_trylock(mtx) == false);
    xylem_mutex_unlock(mtx);

    xylem_cond_destroy(cond);
    xylem_mutex_destroy(mtx);
}

static void test_timedwait_coro_timeout(void) {
    fprintf(stderr, "=== test_timedwait_coro_timeout\n");

    xylem_mutex_t* mtx  = xylem_mutex_create();
    xylem_cond_t*  cond = xylem_cond_create();
    ASSERT(mtx != NULL);
    ASSERT(cond != NULL);

    xylem_mutex_lock(mtx);
    ASSERT(xylem_cond_timedwait(cond, mtx, 20) == false);
    ASSERT(xylem_mutex_trylock(mtx) == false);
    xylem_mutex_unlock(mtx);

    xylem_cond_destroy(cond);
    xylem_mutex_destroy(mtx);
}

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  cond;
    xylem_sem_t*   ready;
    atomic_int     finished;
    uint64_t       timeout_ms;
    bool           result;
    bool           mutex_held;
} _timed_coro_ctx_t;

static void _timed_coro_waiter(void* arg) {
    _timed_coro_ctx_t* ctx = (_timed_coro_ctx_t*)arg;

    xylem_mutex_lock(ctx->mtx);
    xylem_sem_post(ctx->ready);
    ctx->result = xylem_cond_timedwait(ctx->cond, ctx->mtx, ctx->timeout_ms);
    ctx->mutex_held = !xylem_mutex_trylock(ctx->mtx);
    xylem_mutex_unlock(ctx->mtx);
    atomic_store(&ctx->finished, 1);
}

static void test_timedwait_coro_signal(void) {
    fprintf(stderr, "=== test_timedwait_coro_signal\n");

    _timed_coro_ctx_t ctx = {0};
    ctx.mtx               = xylem_mutex_create();
    ctx.cond              = xylem_cond_create();
    ctx.ready             = xylem_sem_create(0);
    ctx.timeout_ms        = UINT64_MAX;
    atomic_init(&ctx.finished, 0);
    ASSERT(ctx.mtx != NULL);
    ASSERT(ctx.cond != NULL);
    ASSERT(ctx.ready != NULL);

    xylem_spawn(_timed_coro_waiter, &ctx);
    xylem_sem_wait(ctx.ready);
    xylem_mutex_lock(ctx.mtx);
    xylem_cond_signal(ctx.cond);
    xylem_mutex_unlock(ctx.mtx);
    while (atomic_load(&ctx.finished) == 0) {
        xylem_sleep(1);
    }

    ASSERT(ctx.result == true);
    ASSERT(ctx.mutex_held == true);
    xylem_sem_destroy(ctx.ready);
    xylem_cond_destroy(ctx.cond);
    xylem_mutex_destroy(ctx.mtx);
}

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  cond;
    xylem_sem_t*   ready;
    uint64_t       timeout_ms;
    bool           result;
    bool           mutex_held;
} _timed_thrd_ctx_t;

static int _timed_thrd_waiter(void* arg) {
    _timed_thrd_ctx_t* ctx = (_timed_thrd_ctx_t*)arg;

    xylem_mutex_lock(ctx->mtx);
    xylem_sem_post(ctx->ready);
    ctx->result =
        xylem_cond_timedwait(ctx->cond, ctx->mtx, ctx->timeout_ms);
    ctx->mutex_held = !xylem_mutex_trylock(ctx->mtx);
    xylem_mutex_unlock(ctx->mtx);
    return 0;
}

static void test_timedwait_thread_timeout(void) {
    fprintf(stderr, "=== test_timedwait_thread_timeout\n");

    _timed_thrd_ctx_t ctx = { .timeout_ms = 20 };
    ctx.mtx   = xylem_mutex_create();
    ctx.cond  = xylem_cond_create();
    ctx.ready = xylem_sem_create(0);
    ASSERT(ctx.mtx != NULL);
    ASSERT(ctx.cond != NULL);
    ASSERT(ctx.ready != NULL);

    thrd_t th;
    ASSERT(thrd_create(&th, _timed_thrd_waiter, &ctx) == thrd_success);
    xylem_sem_wait(ctx.ready);
    ASSERT(thrd_join(th, NULL) == thrd_success);

    ASSERT(ctx.result == false);
    ASSERT(ctx.mutex_held == true);
    xylem_sem_destroy(ctx.ready);
    xylem_cond_destroy(ctx.cond);
    xylem_mutex_destroy(ctx.mtx);
}

static void test_timedwait_thread_signal(void) {
    fprintf(stderr, "=== test_timedwait_thread_signal\n");

    _timed_thrd_ctx_t ctx = { .timeout_ms = UINT64_MAX };
    ctx.mtx   = xylem_mutex_create();
    ctx.cond  = xylem_cond_create();
    ctx.ready = xylem_sem_create(0);
    ASSERT(ctx.mtx != NULL);
    ASSERT(ctx.cond != NULL);
    ASSERT(ctx.ready != NULL);

    thrd_t th;
    ASSERT(thrd_create(&th, _timed_thrd_waiter, &ctx) == thrd_success);
    xylem_sem_wait(ctx.ready);
    xylem_mutex_lock(ctx.mtx);
    xylem_cond_signal(ctx.cond);
    xylem_mutex_unlock(ctx.mtx);
    ASSERT(thrd_join(th, NULL) == thrd_success);

    ASSERT(ctx.result == true);
    ASSERT(ctx.mutex_held == true);
    xylem_sem_destroy(ctx.ready);
    xylem_cond_destroy(ctx.cond);
    xylem_mutex_destroy(ctx.mtx);
}

static void test_timedwait_thread_race(void) {
    fprintf(stderr, "=== test_timedwait_thread_race\n");

    for (int round = 0; round < 100; round++) {
        _timed_thrd_ctx_t ctx = { .timeout_ms = 1 };
        ctx.mtx   = xylem_mutex_create();
        ctx.cond  = xylem_cond_create();
        ctx.ready = xylem_sem_create(0);
        ASSERT(ctx.mtx != NULL);
        ASSERT(ctx.cond != NULL);
        ASSERT(ctx.ready != NULL);

        thrd_t th;
        ASSERT(thrd_create(&th, _timed_thrd_waiter, &ctx) == thrd_success);
        xylem_sem_wait(ctx.ready);
        xylem_mutex_lock(ctx.mtx);
        xylem_cond_signal(ctx.cond);
        xylem_mutex_unlock(ctx.mtx);
        ASSERT(thrd_join(th, NULL) == thrd_success);
        ASSERT(ctx.mutex_held == true);

        xylem_sem_destroy(ctx.ready);
        xylem_cond_destroy(ctx.cond);
        xylem_mutex_destroy(ctx.mtx);
    }
}

typedef struct {
    xylem_mutex_t* cond_mtx;
    xylem_cond_t*  cond;
    atomic_int     ready;
    atomic_int     finished;
} _timed_broadcast_ctx_t;

static void _timed_broadcast_waiter(void* arg) {
    _timed_broadcast_ctx_t* ctx = (_timed_broadcast_ctx_t*)arg;

    xylem_mutex_lock(ctx->cond_mtx);
    atomic_fetch_add(&ctx->ready, 1);
    xylem_cond_timedwait(ctx->cond, ctx->cond_mtx, 2);
    xylem_mutex_unlock(ctx->cond_mtx);
    atomic_fetch_add(&ctx->finished, 1);
}

static int _timed_broadcast_thread(void* arg) {
    _timed_broadcast_ctx_t* ctx = (_timed_broadcast_ctx_t*)arg;

    while (atomic_load(&ctx->ready) < TIMED_BROADCAST_WAITERS) {
        thrd_yield();
    }
    xylem_cond_broadcast(ctx->cond);
    return 0;
}

static void test_timedwait_coro_broadcast_race(void) {
    fprintf(stderr, "=== test_timedwait_coro_broadcast_race\n");

    for (int round = 0; round < 20; round++) {
        _timed_broadcast_ctx_t ctx = {0};
        ctx.cond_mtx               = xylem_mutex_create();
        ctx.cond                   = xylem_cond_create();
        atomic_init(&ctx.ready, 0);
        atomic_init(&ctx.finished, 0);
        ASSERT(ctx.cond_mtx != NULL);
        ASSERT(ctx.cond != NULL);

        thrd_t broadcaster;
        ASSERT(
            thrd_create(&broadcaster, _timed_broadcast_thread, &ctx) ==
            thrd_success);
        for (int i = 0; i < TIMED_BROADCAST_WAITERS; i++) {
            xylem_spawn(_timed_broadcast_waiter, &ctx);
        }
        while (atomic_load(&ctx.finished) < TIMED_BROADCAST_WAITERS) {
            xylem_sleep(1);
        }
        ASSERT(thrd_join(broadcaster, NULL) == thrd_success);

        xylem_mutex_lock(ctx.cond_mtx);
        ASSERT(xylem_cond_timedwait(ctx.cond, ctx.cond_mtx, 1) == false);
        xylem_mutex_unlock(ctx.cond_mtx);

        xylem_cond_destroy(ctx.cond);
        xylem_mutex_destroy(ctx.cond_mtx);
    }
}

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  cond;
    int            flag;
    atomic_int     finished;
    atomic_int     tested;
} _c_one_ctx_t;

static void _c_one_finish(_c_one_ctx_t* ctx) {
    atomic_fetch_add(&ctx->finished, 1);
}

static void _c_one_waiter(void* arg) {
    _c_one_ctx_t* ctx = (_c_one_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    while (!ctx->flag) {
        xylem_cond_wait(ctx->cond, ctx->mtx);
    }
    xylem_mutex_unlock(ctx->mtx);
    atomic_store(&ctx->tested, 1);
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
    ctx->mtx  = xylem_mutex_create();
    ctx->cond = xylem_cond_create();
    xylem_spawn(_c_one_waiter, ctx);
    xylem_spawn(_c_one_signaler, ctx);
}

static void test_signal_one(void) {
    fprintf(stderr, "=== test_signal_one\n");
    for (int round = 0; round < 20; round++) {
        _c_one_ctx_t ctx = {0};
        atomic_init(&ctx.finished, 0);
        atomic_init(&ctx.tested, 0);
        _test_c_one_main(&ctx);
        while (atomic_load(&ctx.finished) < 2) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
        xylem_cond_destroy(ctx.cond);
        xylem_mutex_destroy(ctx.mtx);
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
    atomic_int     signaler_done;
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

    atomic_fetch_add(&ctx->released, 1);
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
    atomic_store(&ctx->signaler_done, 1);
}

static void _test_c_bcast_main(void* arg) {
    _c_bcast_ctx_t* ctx = (_c_bcast_ctx_t*)arg;
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
        atomic_init(&ctx.released, 0);
        atomic_init(&ctx.signaler_done, 0);
        _test_c_bcast_main(&ctx);
        while (atomic_load(&ctx.released) < BCAST_WAITERS ||
               atomic_load(&ctx.signaler_done) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.released) == BCAST_WAITERS);
        xylem_cond_destroy(ctx.all_parked);
        xylem_cond_destroy(ctx.cond);
        xylem_mutex_destroy(ctx.mtx);
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
    atomic_int     producers_done;
    atomic_int     consumed;
    atomic_int     sum;
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
    atomic_fetch_add(&ctx->producers_done, 1);
}

static void _c_bq_consumer(void* arg) {
    _c_bq_ctx_t* ctx = (_c_bq_ctx_t*)arg;
    int v;
    int local = 0;
    while (_c_bq_pop(ctx, &v)) {
        local += v;
    }
    atomic_fetch_add(&ctx->sum, local);
    atomic_fetch_add(&ctx->consumed, 1);
}

static void _test_c_bq_main(void* arg) {
    _c_bq_ctx_t* ctx = (_c_bq_ctx_t*)arg;
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
        atomic_init(&ctx.produced, 0);
        atomic_init(&ctx.producers_done, 0);
        atomic_init(&ctx.consumed, 0);
        atomic_init(&ctx.sum, 0);
        _test_c_bq_main(&ctx);
        while (atomic_load(&ctx.producers_done) < BQ_PRODUCERS ||
               atomic_load(&ctx.consumed) < BQ_CONSUMERS) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.sum) == BQ_PRODUCERS * BQ_PER_PROD);
        xylem_cond_destroy(ctx.not_full);
        xylem_cond_destroy(ctx.not_empty);
        xylem_mutex_destroy(ctx.mtx);
    }
}

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  cond;
    xylem_cond_t*  parked_cond;
    int            parked;
    int            ready;
    atomic_int     finished;
    atomic_int     tested;
} _c_ext_ctx_t;

static void _c_ext_external(void* arg) {
    _c_ext_ctx_t* ctx = (_c_ext_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    ctx->ready = 1;
    xylem_cond_signal(ctx->cond);
    xylem_mutex_unlock(ctx->mtx);
}

static void _c_ext_waiter(void* arg) {
    _c_ext_ctx_t* ctx = (_c_ext_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    ctx->parked = 1;
    xylem_cond_signal(ctx->parked_cond);
    while (!ctx->ready) {
        xylem_cond_wait(ctx->cond, ctx->mtx);
    }
    xylem_mutex_unlock(ctx->mtx);
    atomic_store(&ctx->tested, 1);
    atomic_fetch_add(&ctx->finished, 1);
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
    atomic_fetch_add(&ctx->finished, 1);
}

static void _test_c_ext_main(void* arg) {
    _c_ext_ctx_t* ctx = (_c_ext_ctx_t*)arg;
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
        atomic_init(&ctx.finished, 0);
        atomic_init(&ctx.tested, 0);
        _test_c_ext_main(&ctx);
        while (atomic_load(&ctx.finished) < 2) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
        xylem_cond_destroy(ctx.parked_cond);
        xylem_cond_destroy(ctx.cond);
        xylem_mutex_destroy(ctx.mtx);
    }
}

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  cond;
    thrd_t         thread;
    int            flag;
    atomic_int     thread_released;
    atomic_int     tested;
} _ctw_ctx_t;

static int _ctw_thread_waiter(void* arg) {
    _ctw_ctx_t* ctx = (_ctw_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    while (!ctx->flag) {
        xylem_cond_wait(ctx->cond, ctx->mtx);
    }
    xylem_mutex_unlock(ctx->mtx);
    atomic_store(&ctx->thread_released, 1);
    return 0;
}

static void _ctw_signaler(void* arg) {
    _ctw_ctx_t* ctx = (_ctw_ctx_t*)arg;
    xylem_sleep(30);
    xylem_mutex_lock(ctx->mtx);
    ctx->flag = 1;
    xylem_cond_signal(ctx->cond);
    xylem_mutex_unlock(ctx->mtx);
    while (atomic_load(&ctx->thread_released) == 0) {
        xylem_sleep(2);
    }
    ASSERT(thrd_join(ctx->thread, NULL) == thrd_success);
    atomic_store(&ctx->tested, 1);
}

static void _test_ctw_main(void* arg) {
    _ctw_ctx_t* ctx = (_ctw_ctx_t*)arg;
    ctx->mtx  = xylem_mutex_create();
    ctx->cond = xylem_cond_create();
    ASSERT(
        thrd_create(&ctx->thread, _ctw_thread_waiter, ctx) == thrd_success);
    xylem_spawn(_ctw_signaler, ctx);
}

static void test_thread_waiter(void) {
    fprintf(stderr, "=== test_thread_waiter\n");
    for (int round = 0; round < 10; round++) {
        _ctw_ctx_t ctx = {0};
        atomic_init(&ctx.thread_released, 0);
        atomic_init(&ctx.tested, 0);
        _test_ctw_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
        xylem_cond_destroy(ctx.cond);
        xylem_mutex_destroy(ctx.mtx);
    }
}

#define MIXB_CORO_WAITERS 16

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  cond;
    thrd_t         thread;
    int            flag;
    atomic_int     coro_parked;
    atomic_int     thread_parked;
    atomic_int     coro_released;
    atomic_int     thread_released;
    atomic_int     tested;
} _mixb_ctx_t;

static int _mixb_thread_waiter(void* arg) {
    _mixb_ctx_t* ctx = (_mixb_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    atomic_fetch_add(&ctx->thread_parked, 1);
    while (!ctx->flag) {
        xylem_cond_wait(ctx->cond, ctx->mtx);
    }
    xylem_mutex_unlock(ctx->mtx);
    atomic_fetch_add(&ctx->thread_released, 1);
    return 0;
}

static void _mixb_coro_waiter(void* arg) {
    _mixb_ctx_t* ctx = (_mixb_ctx_t*)arg;
    xylem_mutex_lock(ctx->mtx);
    atomic_fetch_add(&ctx->coro_parked, 1);
    while (!ctx->flag) {
        xylem_cond_wait(ctx->cond, ctx->mtx);
    }
    xylem_mutex_unlock(ctx->mtx);
    atomic_fetch_add(&ctx->coro_released, 1);
}

static void _mixb_driver(void* arg) {
    _mixb_ctx_t* ctx = (_mixb_ctx_t*)arg;
    while (atomic_load(&ctx->coro_parked) < MIXB_CORO_WAITERS ||
           atomic_load(&ctx->thread_parked) < 1) {
        xylem_sleep(2);
    }
    xylem_mutex_lock(ctx->mtx);
    ctx->flag = 1;
    xylem_cond_broadcast(ctx->cond);
    xylem_mutex_unlock(ctx->mtx);
    while (atomic_load(&ctx->coro_released) < MIXB_CORO_WAITERS ||
           atomic_load(&ctx->thread_released) < 1) {
        xylem_sleep(2);
    }
    ASSERT(thrd_join(ctx->thread, NULL) == thrd_success);
    atomic_store(&ctx->tested, 1);
}

static void _test_mixb_main(void* arg) {
    _mixb_ctx_t* ctx = (_mixb_ctx_t*)arg;
    ctx->mtx  = xylem_mutex_create();
    ctx->cond = xylem_cond_create();
    ASSERT(
        thrd_create(&ctx->thread, _mixb_thread_waiter, ctx) == thrd_success);
    for (int i = 0; i < MIXB_CORO_WAITERS; i++) {
        xylem_spawn(_mixb_coro_waiter, ctx);
    }
    xylem_spawn(_mixb_driver, ctx);
}

static void test_mixed_broadcast(void) {
    fprintf(stderr, "=== test_mixed_broadcast\n");
    for (int round = 0; round < 10; round++) {
        _mixb_ctx_t ctx = {0};
        atomic_init(&ctx.coro_parked, 0);
        atomic_init(&ctx.thread_parked, 0);
        atomic_init(&ctx.coro_released, 0);
        atomic_init(&ctx.thread_released, 0);
        atomic_init(&ctx.tested, 0);
        _test_mixb_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
        xylem_cond_destroy(ctx.cond);
        xylem_mutex_destroy(ctx.mtx);
    }
}

typedef struct {
    xylem_mutex_t* mtx;
    xylem_cond_t*  cond;
    xylem_cond_t*  all_parked;
    int            flag;
    int            parked;
    atomic_int     observed_yield;
    atomic_int     released;
    atomic_int     signaler_done;
    int            tested;
} _credit_ctx_t;

static void _credit_maybe_finish(_credit_ctx_t* ctx) {
    if (atomic_load(&ctx->released) != CREDIT_WAKE_COUNT ||
        atomic_load(&ctx->signaler_done) == 0) {
        return;
    }
    ctx->tested = 1;
    xylem_cond_destroy(ctx->all_parked);
    ctx->all_parked = NULL;
    xylem_cond_destroy(ctx->cond);
    ctx->cond = NULL;
    xylem_mutex_destroy(ctx->mtx);
    ctx->mtx = NULL;
}

static void _credit_waiter(void* arg) {
    _credit_ctx_t* ctx = (_credit_ctx_t*)arg;

    xylem_mutex_lock(ctx->mtx);
    if (++ctx->parked == CREDIT_WAKE_COUNT) {
        xylem_cond_signal(ctx->all_parked);
    }
    while (!ctx->flag) {
        xylem_cond_wait(ctx->cond, ctx->mtx);
    }
    xylem_mutex_unlock(ctx->mtx);

    atomic_fetch_add(&ctx->released, 1);
    _credit_maybe_finish(ctx);
}

static void _credit_observer(void* arg) {
    _credit_ctx_t* ctx = (_credit_ctx_t*)arg;

    if (atomic_load(&ctx->signaler_done) == 0) {
        atomic_store(&ctx->observed_yield, 1);
    }
}

static void _credit_signaler(void* arg) {
    _credit_ctx_t* ctx = (_credit_ctx_t*)arg;

    xylem_mutex_lock(ctx->mtx);
    while (ctx->parked < CREDIT_WAKE_COUNT) {
        xylem_cond_wait(ctx->all_parked, ctx->mtx);
    }

    ctx->flag = 1;
    xylem_spawn(_credit_observer, ctx);
    for (int i = 0; i < CREDIT_WAKE_COUNT; i++) {
        xylem_cond_signal(ctx->cond);
    }
    ASSERT(atomic_load(&ctx->observed_yield) == 1);
    atomic_store(&ctx->signaler_done, 1);
    xylem_mutex_unlock(ctx->mtx);

    _credit_maybe_finish(ctx);
}

static void _test_credit_main(void* arg) {
    _credit_ctx_t* ctx = (_credit_ctx_t*)arg;
    ctx->mtx        = xylem_mutex_create();
    ctx->cond       = xylem_cond_create();
    ctx->all_parked = xylem_cond_create();
    for (int i = 0; i < CREDIT_WAKE_COUNT; i++) {
        xylem_spawn(_credit_waiter, ctx);
    }
    xylem_spawn(_credit_signaler, ctx);
}

static void test_signal_consumes_credit(void) {
    fprintf(stderr, "=== test_signal_consumes_credit\n");

    _credit_ctx_t ctx = {0};
    _test_credit_main(&ctx);
    while (ctx.tested == 0) {
        xylem_sleep(1);
    }
    ASSERT(ctx.tested == 1);
}

static void _test_run_functional(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_timedwait_zero();
    test_timedwait_coro_timeout();
    test_timedwait_coro_signal();
    test_timedwait_thread_timeout();
    test_timedwait_thread_signal();
    test_timedwait_thread_race();
    test_timedwait_coro_broadcast_race();
    test_signal_one();
    test_broadcast();
    test_bounded_queue();
    test_external_signal();
    test_thread_waiter();
    test_mixed_broadcast();
    _utils_watchdog_stop();
    xylem_shutdown();
}

static void _test_run_credit(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_signal_consumes_credit();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_functional, NULL, &_functional_rt_opts);
    xylem_run(_test_run_credit, NULL, &_credit_rt_opts);
    return 0;
}
