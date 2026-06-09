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
#include "xylem/sync/xylem-sem.h"
#include "assert.h"
#include "thrds.h"
#include "utils.h"

#include <stdatomic.h>
#include <stdio.h>

#define SAFETY_TIMEOUT_MS 10000

static xylem_opts_t _rt_opts = { .workers = 4 };

/* ------------------------------------------------------------------ */
/* test_count: initial count grants tokens without blocking.           */
/* ------------------------------------------------------------------ */

typedef struct {
    xylem_sem_t* sem;
    int          tested;
} _count_ctx_t;

static void _count_main(void* arg) {
    _count_ctx_t* ctx = (_count_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    ctx->sem = xylem_sem_create(2);
    /* Two tokens available: two waits return immediately. */
    xylem_sem_wait(ctx->sem);
    xylem_sem_wait(ctx->sem);
    /* Third would block; timedwait(0) must report empty. */
    ASSERT(xylem_sem_timedwait(ctx->sem, 0) == false);
    /* Post then grab. */
    xylem_sem_post(ctx->sem);
    ASSERT(xylem_sem_timedwait(ctx->sem, 0) == true);

    ctx->tested = 1;
    xylem_sem_destroy(ctx->sem);
    xylem_shutdown();
}

static void test_count(void) {
    fprintf(stderr, "=== test_count\n");
    _count_ctx_t ctx = {0};
    xylem_run(_count_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* ------------------------------------------------------------------ */
/* test_coro_pair: one coroutine blocks on wait, another posts.        */
/* ------------------------------------------------------------------ */

typedef struct {
    xylem_sem_t* sem;
    atomic_int   woke;
    int          tested;
} _pair_ctx_t;

static void _pair_waiter(void* arg) {
    _pair_ctx_t* ctx = (_pair_ctx_t*)arg;
    xylem_sem_wait(ctx->sem);
    atomic_store(&ctx->woke, 1);
    ctx->tested = 1;
    xylem_sem_destroy(ctx->sem);
    xylem_shutdown();
}

static void _pair_poster(void* arg) {
    _pair_ctx_t* ctx = (_pair_ctx_t*)arg;
    /* Give the waiter time to park first. */
    xylem_sleep(50);
    ASSERT(atomic_load(&ctx->woke) == 0);
    xylem_sem_post(ctx->sem);
}

static void _pair_main(void* arg) {
    _pair_ctx_t* ctx = (_pair_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->sem = xylem_sem_create(0);
    xylem_spawn(_pair_waiter, ctx);
    xylem_spawn(_pair_poster, ctx);
}

static void test_coro_pair(void) {
    fprintf(stderr, "=== test_coro_pair\n");
    _pair_ctx_t ctx = {0};
    atomic_init(&ctx.woke, 0);
    xylem_run(_pair_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* ------------------------------------------------------------------ */
/* test_coro_posts_thread: coroutine posts, external thread waits.     */
/* ------------------------------------------------------------------ */

typedef struct {
    xylem_sem_t* sem;
    atomic_int   thread_done;
    int          tested;
} _ct_ctx_t;

static int _ct_thread_fn(void* arg) {
    _ct_ctx_t* ctx = (_ct_ctx_t*)arg;
    /* Blocks on the OS path until the coroutine posts. */
    xylem_sem_wait(ctx->sem);
    atomic_store(&ctx->thread_done, 1);
    return 0;
}

static void _ct_poster(void* arg) {
    _ct_ctx_t* ctx = (_ct_ctx_t*)arg;
    xylem_sleep(50);
    xylem_sem_post(ctx->sem);
    /* Spin-wait (in coroutine time) for the thread to observe the wake. */
    while (atomic_load(&ctx->thread_done) == 0) {
        xylem_sleep(5);
    }
    ctx->tested = 1;
    xylem_sem_destroy(ctx->sem);
    xylem_shutdown();
}

static void _ct_main(void* arg) {
    _ct_ctx_t* ctx = (_ct_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->sem = xylem_sem_create(0);
    thrd_t th;
    thrd_create(&th, _ct_thread_fn, ctx);
    thrd_detach(th);
    xylem_spawn(_ct_poster, ctx);
}

static void test_coro_posts_thread(void) {
    fprintf(stderr, "=== test_coro_posts_thread\n");
    _ct_ctx_t ctx = {0};
    atomic_init(&ctx.thread_done, 0);
    xylem_run(_ct_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* ------------------------------------------------------------------ */
/* test_thread_posts_coro: external thread posts, coroutine waits.     */
/* ------------------------------------------------------------------ */

typedef struct {
    xylem_sem_t* sem;
    int          tested;
} _tc_ctx_t;

static int _tc_thread_fn(void* arg) {
    _tc_ctx_t* ctx = (_tc_ctx_t*)arg;
    /* Sleep a bit so the coroutine parks first, then post across the
     * boundary -- this reschedules the parked coroutine. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    thrd_sleep(&ts, NULL);
    xylem_sem_post(ctx->sem);
    return 0;
}

static void _tc_waiter(void* arg) {
    _tc_ctx_t* ctx = (_tc_ctx_t*)arg;
    thrd_t th;
    thrd_create(&th, _tc_thread_fn, ctx);
    thrd_detach(th);

    xylem_sem_wait(ctx->sem); /* parks until the external thread posts */
    ctx->tested = 1;
    xylem_sem_destroy(ctx->sem);
    xylem_shutdown();
}

static void _tc_main(void* arg) {
    _tc_ctx_t* ctx = (_tc_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->sem = xylem_sem_create(0);
    xylem_spawn(_tc_waiter, ctx);
}

static void test_thread_posts_coro(void) {
    fprintf(stderr, "=== test_thread_posts_coro\n");
    _tc_ctx_t ctx = {0};
    xylem_run(_tc_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* ------------------------------------------------------------------ */
/* test_coro_timeout: timedwait on an empty sem times out.             */
/* ------------------------------------------------------------------ */

typedef struct {
    int tested;
} _coto_ctx_t;

static void _coto_main(void* arg) {
    _coto_ctx_t* ctx = (_coto_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    xylem_sem_t* sem = xylem_sem_create(0);

    uint64_t t0 = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    bool ok = xylem_sem_timedwait(sem, 100);
    uint64_t t1 = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    ASSERT(ok == false);          /* timed out */
    ASSERT((t1 - t0) >= 90);      /* actually waited ~100ms */

    /* Sem still usable after a timeout: post + grab. */
    xylem_sem_post(sem);
    ASSERT(xylem_sem_timedwait(sem, 100) == true);

    ctx->tested = 1;
    xylem_sem_destroy(sem);
    xylem_shutdown();
}

static void test_coro_timeout(void) {
    fprintf(stderr, "=== test_coro_timeout\n");
    _coto_ctx_t ctx = {0};
    xylem_run(_coto_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* ------------------------------------------------------------------ */
/* test_coro_timedwait_wins: post arrives before the deadline.         */
/* ------------------------------------------------------------------ */

typedef struct {
    xylem_sem_t* sem;
    int          tested;
} _win_ctx_t;

static void _win_waiter(void* arg) {
    _win_ctx_t* ctx = (_win_ctx_t*)arg;
    bool ok = xylem_sem_timedwait(ctx->sem, 5000);
    ASSERT(ok == true); /* posted well before the 5s deadline */
    ctx->tested = 1;
    xylem_sem_destroy(ctx->sem);
    xylem_shutdown();
}

static void _win_poster(void* arg) {
    _win_ctx_t* ctx = (_win_ctx_t*)arg;
    xylem_sleep(30);
    xylem_sem_post(ctx->sem);
}

static void _win_main(void* arg) {
    _win_ctx_t* ctx = (_win_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->sem = xylem_sem_create(0);
    xylem_spawn(_win_waiter, ctx);
    xylem_spawn(_win_poster, ctx);
}

static void test_coro_timedwait_wins(void) {
    fprintf(stderr, "=== test_coro_timedwait_wins\n");
    _win_ctx_t ctx = {0};
    xylem_run(_win_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* ------------------------------------------------------------------ */
/* test_thread_timeout: external-thread timedwait times out.           */
/* ------------------------------------------------------------------ */

typedef struct {
    xylem_sem_t* sem;
    atomic_int   thread_result; /* 0 unset, 1 timed-out as expected     */
    int          tested;
} _tto_ctx_t;

static int _tto_thread_fn(void* arg) {
    _tto_ctx_t* ctx = (_tto_ctx_t*)arg;
    bool ok = xylem_sem_timedwait(ctx->sem, 100);
    atomic_store(&ctx->thread_result, ok ? 2 : 1);
    return 0;
}

static void _tto_driver(void* arg) {
    _tto_ctx_t* ctx = (_tto_ctx_t*)arg;
    while (atomic_load(&ctx->thread_result) == 0) {
        xylem_sleep(5);
    }
    ASSERT(atomic_load(&ctx->thread_result) == 1); /* timed out */
    ctx->tested = 1;
    xylem_sem_destroy(ctx->sem);
    xylem_shutdown();
}

static void _tto_main(void* arg) {
    _tto_ctx_t* ctx = (_tto_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->sem = xylem_sem_create(0);
    thrd_t th;
    thrd_create(&th, _tto_thread_fn, ctx);
    thrd_detach(th);
    xylem_spawn(_tto_driver, ctx);
}

static void test_thread_timeout(void) {
    fprintf(stderr, "=== test_thread_timeout\n");
    _tto_ctx_t ctx = {0};
    atomic_init(&ctx.thread_result, 0);
    xylem_run(_tto_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* ------------------------------------------------------------------ */
/* test_stress: N posters, N coroutine waiters, every token accounted. */
/* ------------------------------------------------------------------ */

#define STRESS_WAITERS 16
#define STRESS_ROUNDS  100

typedef struct {
    xylem_sem_t* sem;
    atomic_int   acquired;
    atomic_int   done;
    int          tested;
} _stress_ctx_t;

static void _stress_waiter(void* arg) {
    _stress_ctx_t* ctx = (_stress_ctx_t*)arg;
    for (int i = 0; i < STRESS_ROUNDS; i++) {
        xylem_sem_wait(ctx->sem);
        atomic_fetch_add(&ctx->acquired, 1);
    }
    if (atomic_fetch_add(&ctx->done, 1) + 1 == STRESS_WAITERS) {
        ASSERT(atomic_load(&ctx->acquired) == STRESS_WAITERS * STRESS_ROUNDS);
        ctx->tested = 1;
        xylem_sem_destroy(ctx->sem);
        xylem_shutdown();
    }
}

static void _stress_poster(void* arg) {
    _stress_ctx_t* ctx = (_stress_ctx_t*)arg;
    for (int i = 0; i < STRESS_ROUNDS; i++) {
        xylem_sem_post(ctx->sem);
        if ((i & 7) == 0) {
            xylem_sleep(1);
        }
    }
}

static void _stress_main(void* arg) {
    _stress_ctx_t* ctx = (_stress_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->sem = xylem_sem_create(0);
    for (int i = 0; i < STRESS_WAITERS; i++) {
        xylem_spawn(_stress_waiter, ctx);
    }
    for (int i = 0; i < STRESS_WAITERS; i++) {
        xylem_spawn(_stress_poster, ctx);
    }
}

static void test_stress(void) {
    fprintf(stderr, "=== test_stress\n");
    _stress_ctx_t ctx = {0};
    atomic_init(&ctx.acquired, 0);
    atomic_init(&ctx.done, 0);
    xylem_run(_stress_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

int main(void) {
    test_count();
    test_coro_pair();
    test_coro_posts_thread();
    test_thread_posts_coro();
    test_coro_timeout();
    test_coro_timedwait_wins();
    test_thread_timeout();
    test_stress();
    return 0;
}
