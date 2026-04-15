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

#include <stdatomic.h>

#define THREAD_COUNT    10
#define STRESS_THREADS  20

typedef struct {
    xylem_waitgroup_t* wg;
    atomic_int*        completed;
    atomic_int*        started;
} _worker_ctx_t;

static int _worker_thread(void* arg) {
    _worker_ctx_t* ctx = (_worker_ctx_t*)arg;
    atomic_fetch_add(ctx->started, 1);
    /* Yield to let other threads start — no sleep needed. */
    thrd_yield();
    atomic_fetch_add(ctx->completed, 1);
    xylem_waitgroup_done(ctx->wg);
    return 0;
}

static int _early_done_thread(void* arg) {
    xylem_waitgroup_t* wg = (xylem_waitgroup_t*)arg;
    xylem_waitgroup_done(wg);
    return 0;
}

static int _stress_thread(void* arg) {
    xylem_waitgroup_t* wg = (xylem_waitgroup_t*)arg;
    volatile int       dummy = 0;
    int                iters = 10 + (rand() % 50);
    for (int i = 0; i < iters; i++) {
        dummy += i;
    }
    xylem_waitgroup_done(wg);
    return 0;
}

/* Create and immediately destroy. */
static void test_create_destroy(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    xylem_waitgroup_destroy(wg);
}

/* Basic add/done/wait without threads. */
static void test_basic_operations(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);

    xylem_waitgroup_add(wg, 0);
    xylem_waitgroup_done(wg);

    xylem_waitgroup_add(wg, 3);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_wait(wg);

    xylem_waitgroup_destroy(wg);
}

/* Multiple threads: wait blocks until all done. */
static void test_multiple_threads(void) {
    thrd_t threads[THREAD_COUNT];
    _worker_ctx_t ctxs[THREAD_COUNT];
    atomic_int completed = 0;
    atomic_int started = 0;

    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    xylem_waitgroup_add(wg, THREAD_COUNT);

    for (int i = 0; i < THREAD_COUNT; i++) {
        ctxs[i].wg        = wg;
        ctxs[i].completed = &completed;
        ctxs[i].started   = &started;
        ASSERT(thrd_create(&threads[i], _worker_thread, &ctxs[i]) == thrd_success);
    }

    while (atomic_load(&started) < THREAD_COUNT) {
        thrd_yield();
    }

    xylem_waitgroup_wait(wg);
    ASSERT(atomic_load(&completed) == THREAD_COUNT);

    /* Second wait should return immediately. */
    xylem_waitgroup_wait(wg);

    for (int i = 0; i < THREAD_COUNT; i++) {
        thrd_join(threads[i], NULL);
    }
    xylem_waitgroup_destroy(wg);
}

/* Thread calls done before main calls wait. */
static void test_early_done(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);

    thrd_t thread;
    xylem_waitgroup_add(wg, 1);
    ASSERT(thrd_create(&thread, _early_done_thread, wg) == thrd_success);

    xylem_waitgroup_wait(wg);
    thrd_join(thread, NULL);
    xylem_waitgroup_destroy(wg);
}

/* Done without prior add (counter goes negative). */
static void test_repeated_done(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);

    xylem_waitgroup_done(wg);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_done(wg);
    xylem_waitgroup_wait(wg);

    xylem_waitgroup_destroy(wg);
}

/* Large delta: add 1000, done 1000 times. */
static void test_large_delta(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);

    const size_t N = 1000;
    xylem_waitgroup_add(wg, N);
    for (size_t i = 0; i < N; i++) {
        xylem_waitgroup_done(wg);
    }
    xylem_waitgroup_wait(wg);

    xylem_waitgroup_destroy(wg);
}

/* Concurrent stress: many threads with variable work. */
static void test_concurrent_stress(void) {
    srand((unsigned int)time(NULL));
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);

    thrd_t threads[STRESS_THREADS];
    xylem_waitgroup_add(wg, STRESS_THREADS);

    for (int i = 0; i < STRESS_THREADS; i++) {
        ASSERT(thrd_create(&threads[i], _stress_thread, wg) == thrd_success);
    }

    xylem_waitgroup_wait(wg);

    for (int i = 0; i < STRESS_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }
    xylem_waitgroup_destroy(wg);
}

int main(void) {
    test_create_destroy();
    test_basic_operations();
    test_repeated_done();
    test_large_delta();
    test_multiple_threads();
    test_early_done();
    test_concurrent_stress();
    return 0;
}
