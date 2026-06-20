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
#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdbool.h>

#define SUBMITTERS 64

typedef struct {
    dynpool_t*  pool;
    atomic_bool start;
    atomic_bool release;
    atomic_int  active;
    atomic_int  done;
    atomic_int  max_active;
} _max_ctx_t;

static void _max_update(atomic_int* max_value, int value) {
    int cur = atomic_load_explicit(max_value, memory_order_relaxed);
    while (value > cur &&
           !atomic_compare_exchange_weak_explicit(
               max_value,
               &cur,
               value,
               memory_order_relaxed,
               memory_order_relaxed)) {
    }
}

static void _max_job(void* arg) {
    _max_ctx_t* ctx = (_max_ctx_t*)arg;
    int active = atomic_fetch_add_explicit(
        &ctx->active, 1, memory_order_acq_rel) + 1;
    _max_update(&ctx->max_active, active);

    while (!atomic_load_explicit(&ctx->release, memory_order_acquire)) {
        thrd_yield();
    }

    atomic_fetch_sub_explicit(&ctx->active, 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&ctx->done, 1, memory_order_acq_rel);
}

static int _max_submitter(void* arg) {
    _max_ctx_t* ctx = (_max_ctx_t*)arg;
    while (!atomic_load_explicit(&ctx->start, memory_order_acquire)) {
        thrd_yield();
    }
    ASSERT(dynpool_submit(ctx->pool, _max_job, ctx) == 0);
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

    atomic_store_explicit(&ctx.start, true, memory_order_release);
    for (int i = 0; i < SUBMITTERS; i++) {
        ASSERT(thrd_join(threads[i], NULL) == thrd_success);
    }

    for (int i = 0; i < 10000; i++) {
        thrd_yield();
        if (atomic_load_explicit(&ctx.max_active, memory_order_acquire) > 1) {
            break;
        }
    }

    atomic_store_explicit(&ctx.release, true, memory_order_release);
    while (atomic_load_explicit(&ctx.done, memory_order_acquire) < SUBMITTERS) {
        thrd_yield();
    }

    ASSERT(atomic_load_explicit(&ctx.max_active, memory_order_acquire) == 1);

    dynpool_destroy(ctx.pool);
}

int main(void) {
    test_max_threads_is_enforced_under_concurrent_submit();
    return 0;
}
