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

#include "runtime/dynpool.h"
#include "assert.h"
#include "xylem/xylem-utils.h"
#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdbool.h>

#define SUBMITTERS 64
#define RETIRE_SUBMITTERS 8
#define RETIRE_ROUNDS 256

typedef struct {
    dynpool_t*  pool;
    atomic_bool start;
    atomic_bool release;
    atomic_int  active;
    atomic_int  done;
    atomic_int  max_active;
} _max_ctx_t;

typedef struct {
    dynpool_t*  pool;
    atomic_bool start;
    atomic_int  done;
} _retire_ctx_t;

static void _max_update(atomic_int* max_value, int value) {
    int cur = atomic_load(max_value);
    while (value > cur &&
           !atomic_compare_exchange_weak(max_value, &cur, value)) {
    }
}

static void _max_job(void* arg) {
    _max_ctx_t* ctx = (_max_ctx_t*)arg;
    int active = atomic_fetch_add(&ctx->active, 1) + 1;
    _max_update(&ctx->max_active, active);

    while (!atomic_load(&ctx->release)) {
        thrd_yield();
    }

    atomic_fetch_sub(&ctx->active, 1);
    atomic_fetch_add(&ctx->done, 1);
}

static int _max_submitter(void* arg) {
    _max_ctx_t* ctx = (_max_ctx_t*)arg;
    while (!atomic_load(&ctx->start)) {
        thrd_yield();
    }
    ASSERT(dynpool_submit(ctx->pool, _max_job, ctx) == 0);
    return 0;
}

static void _retire_job(void* arg) {
    _retire_ctx_t* ctx = (_retire_ctx_t*)arg;
    atomic_fetch_add(&ctx->done, 1);
}

static int _retire_submitter(void* arg) {
    _retire_ctx_t* ctx = (_retire_ctx_t*)arg;
    while (!atomic_load(&ctx->start)) {
        thrd_yield();
    }

    for (int i = 0; i < RETIRE_ROUNDS; i++) {
        ASSERT(dynpool_submit(ctx->pool, _retire_job, ctx) == 0);
        thrd_yield();
    }
    return 0;
}

static void test_max_threads_is_enforced_under_concurrent_submit(void) {
    dynpool_opts_t opts = {
        .max_threads = 1,
        .idle_timeout = 10000,
    };
    _max_ctx_t ctx = {0};
    atomic_init(&ctx.start, false);
    atomic_init(&ctx.release, false);
    atomic_init(&ctx.active, 0);
    atomic_init(&ctx.done, 0);
    atomic_init(&ctx.max_active, 0);

    ctx.pool = dynpool_create(&opts);
    ASSERT(ctx.pool != NULL);

    thrd_t threads[SUBMITTERS];
    for (int i = 0; i < SUBMITTERS; i++) {
        ASSERT(thrd_create(&threads[i], _max_submitter, &ctx) == thrd_success);
    }

    atomic_store(&ctx.start, true);
    for (int i = 0; i < SUBMITTERS; i++) {
        ASSERT(thrd_join(threads[i], NULL) == thrd_success);
    }

    for (int i = 0; i < 10000; i++) {
        thrd_yield();
        if (atomic_load(&ctx.max_active) > 1) {
            break;
        }
    }

    atomic_store(&ctx.release, true);
    while (atomic_load(&ctx.done) < SUBMITTERS) {
        thrd_yield();
    }

    ASSERT(atomic_load(&ctx.max_active) == 1);

    dynpool_destroy(ctx.pool);
}

static void test_concurrent_submit_while_workers_retire_drains_queue(void) {
    dynpool_opts_t opts = {
        .max_threads = 4,
        .idle_timeout = 1,
    };
    _retire_ctx_t ctx = {0};
    atomic_init(&ctx.start, false);
    atomic_init(&ctx.done, 0);

    ctx.pool = dynpool_create(&opts);
    ASSERT(ctx.pool != NULL);

    thrd_t threads[RETIRE_SUBMITTERS];
    for (int i = 0; i < RETIRE_SUBMITTERS; i++) {
        ASSERT(thrd_create(&threads[i], _retire_submitter, &ctx)
               == thrd_success);
    }

    atomic_store(&ctx.start, true);
    for (int i = 0; i < RETIRE_SUBMITTERS; i++) {
        ASSERT(thrd_join(threads[i], NULL) == thrd_success);
    }

    int expected = RETIRE_SUBMITTERS * RETIRE_ROUNDS;
    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 1000;
    while (atomic_load(&ctx.done) < expected &&
           xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) < deadline) {
        thrd_yield();
    }

    ASSERT(atomic_load(&ctx.done) == expected);

    dynpool_destroy(ctx.pool);
}

int main(void) {
    test_max_threads_is_enforced_under_concurrent_submit();
    test_concurrent_submit_while_workers_retire_drains_queue();
    return 0;
}
