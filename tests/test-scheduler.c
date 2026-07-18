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

#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stdatomic.h>
#include <stdint.h>

#define SCHED_BATCH_COUNT     8
#define SCHED_REUSE_COUNT     128
#define SCHED_STACK_TOUCH_LEN (16 * 1024)
#define SCHED_YIELD_COUNT     64

typedef struct {
    xylem_waitgroup_t* wg;
    atomic_int         runs;
} _run_ctx_t;

typedef struct {
    xylem_waitgroup_t*  wg;
    _Atomic(mco_coro*) waiter;
    atomic_int          runs;
} _park_ctx_t;

static bool _park_wait_commit_cb(mco_coro* co, void* arg) {
    _park_ctx_t* ctx = (_park_ctx_t*)arg;

    atomic_store(&ctx->waiter, co);
    return true;
}

static bool _park_decline_commit_cb(mco_coro* co, void* arg) {
    (void)co;
    (void)arg;
    return false;
}

static void _run_once(void* arg) {
    _run_ctx_t* ctx = (_run_ctx_t*)arg;

    atomic_fetch_add(&ctx->runs, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _touch_stack(void* arg) {
    _run_ctx_t*      ctx = (_run_ctx_t*)arg;
    volatile uint8_t stack[SCHED_STACK_TOUCH_LEN];

    for (size_t i = 0; i < SCHED_STACK_TOUCH_LEN; i += 4096) {
        stack[i] = (uint8_t)i;
    }
    atomic_fetch_add(&ctx->runs, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _yield_repeatedly(void* arg) {
    _run_ctx_t* ctx = (_run_ctx_t*)arg;

    for (int i = 0; i < SCHED_YIELD_COUNT; i++) {
        atomic_fetch_add(&ctx->runs, 1);
        scheduler_coro_yield();
    }
    xylem_waitgroup_done(ctx->wg);
}

static void _park_and_resume(void* arg) {
    _park_ctx_t* ctx = (_park_ctx_t*)arg;

    scheduler_coro_park(
        runtime_get_scheduler(),
        _park_wait_commit_cb,
        ctx);
    atomic_fetch_add(&ctx->runs, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _park_and_decline(void* arg) {
    _run_ctx_t* ctx = (_run_ctx_t*)arg;

    scheduler_coro_park(
        runtime_get_scheduler(),
        _park_decline_commit_cb,
        NULL);
    atomic_fetch_add(&ctx->runs, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void test_spawn_and_exit(void) {
    _run_ctx_t ctx = {
        .wg = xylem_waitgroup_create(),
    };
    ASSERT(ctx.wg != NULL);
    atomic_init(&ctx.runs, 0);
    xylem_waitgroup_add(ctx.wg, 1);

    ASSERT(scheduler_coro_spawn(
               runtime_get_scheduler(),
               _run_once,
               &ctx)
           == 0);
    xylem_waitgroup_wait(ctx.wg);
    ASSERT(atomic_load(&ctx.runs) == 1);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_repeated_yield(void) {
    _run_ctx_t ctx = {
        .wg = xylem_waitgroup_create(),
    };
    ASSERT(ctx.wg != NULL);
    atomic_init(&ctx.runs, 0);
    xylem_waitgroup_add(ctx.wg, 1);

    ASSERT(scheduler_coro_spawn(
               runtime_get_scheduler(),
               _yield_repeatedly,
               &ctx)
           == 0);
    xylem_waitgroup_wait(ctx.wg);
    ASSERT(atomic_load(&ctx.runs) == SCHED_YIELD_COUNT);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_reuse_decommitted_stack(void) {
    _run_ctx_t ctx = {
        .wg = xylem_waitgroup_create(),
    };
    ASSERT(ctx.wg != NULL);
    atomic_init(&ctx.runs, 0);

    for (int i = 0; i < SCHED_REUSE_COUNT; i++) {
        xylem_waitgroup_add(ctx.wg, 1);
        ASSERT(
            scheduler_coro_spawn(runtime_get_scheduler(), _touch_stack, &ctx) ==
            0);
        xylem_waitgroup_wait(ctx.wg);
    }

    ASSERT(atomic_load(&ctx.runs) == SCHED_REUSE_COUNT);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_declined_park(void) {
    _run_ctx_t ctx = {
        .wg = xylem_waitgroup_create(),
    };
    ASSERT(ctx.wg != NULL);
    atomic_init(&ctx.runs, 0);
    xylem_waitgroup_add(ctx.wg, 1);

    ASSERT(scheduler_coro_spawn(
               runtime_get_scheduler(),
               _park_and_decline,
               &ctx)
           == 0);
    xylem_waitgroup_wait(ctx.wg);
    ASSERT(atomic_load(&ctx.runs) == 1);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_external_ready(void) {
    _park_ctx_t ctx = {
        .wg = xylem_waitgroup_create(),
    };
    ASSERT(ctx.wg != NULL);
    atomic_init(&ctx.waiter, NULL);
    atomic_init(&ctx.runs, 0);
    xylem_waitgroup_add(ctx.wg, 1);

    ASSERT(scheduler_coro_spawn(
               runtime_get_scheduler(),
               _park_and_resume,
               &ctx)
           == 0);

    mco_coro* co;
    while (!(co = atomic_exchange(&ctx.waiter, NULL))) {
        runtime_yield();
    }
    scheduler_coro_ready(runtime_get_scheduler(), co);

    xylem_waitgroup_wait(ctx.wg);
    ASSERT(atomic_load(&ctx.runs) == 1);
    xylem_waitgroup_destroy(ctx.wg);
}

static void test_batch_ready(void) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    xylem_waitgroup_add(wg, SCHED_BATCH_COUNT);

    _park_ctx_t items[SCHED_BATCH_COUNT];
    for (int i = 0; i < SCHED_BATCH_COUNT; i++) {
        items[i].wg = wg;
        atomic_init(&items[i].waiter, NULL);
        atomic_init(&items[i].runs, 0);
        ASSERT(scheduler_coro_spawn(
                   runtime_get_scheduler(),
                   _park_and_resume,
                   &items[i])
               == 0);
    }

    mco_coro* coros[SCHED_BATCH_COUNT];
    for (int i = 0; i < SCHED_BATCH_COUNT; i++) {
        while (!(coros[i] = atomic_exchange(&items[i].waiter, NULL))) {
            runtime_yield();
        }
    }
    scheduler_coro_ready_batch(
        runtime_get_scheduler(),
        coros,
        SCHED_BATCH_COUNT);

    xylem_waitgroup_wait(wg);
    for (int i = 0; i < SCHED_BATCH_COUNT; i++) {
        ASSERT(atomic_load(&items[i].runs) == 1);
    }
    xylem_waitgroup_destroy(wg);
}

static void _test_run_all(void* arg) {
    (void)arg;

    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_spawn_and_exit();
    test_repeated_yield();
    test_reuse_decommitted_stack();
    test_declined_park();
    test_external_ready();
    test_batch_ready();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_opts_t one_worker = {.workers = 1};
    xylem_opts_t many_workers = {.workers = 4};

    xylem_run(_test_run_all, NULL, &one_worker);
    xylem_run(_test_run_all, NULL, &many_workers);
    return 0;
}
