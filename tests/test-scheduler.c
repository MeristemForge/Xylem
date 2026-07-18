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

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#define SCHED_BATCH_COUNT      8
#define SCHED_CHURN_TASK_COUNT 4096
#define SCHED_CHURN_DIRECT     2048
#define SCHED_CHURN_EXTERNAL   1024
#define SCHED_CHURN_PRODUCERS  3
#define SCHED_REUSE_COUNT      128
#define SCHED_STACK_TOUCH_LEN  (16 * 1024)
#define SCHED_YIELD_COUNT      64

typedef struct {
    xylem_waitgroup_t* wg;
    atomic_int         runs;
} _run_ctx_t;

typedef struct {
    xylem_waitgroup_t*  wg;
    _Atomic(mco_coro*) waiter;
    atomic_int          runs;
} _park_ctx_t;

typedef struct _churn_ctx_s _churn_ctx_t;

typedef struct {
    _churn_ctx_t* ctx;
    uint32_t      id;
} _churn_task_t;

typedef struct {
    _churn_ctx_t* ctx;
    uint32_t      begin;
    uint32_t      end;
} _churn_range_t;

struct _churn_ctx_s {
    xylem_waitgroup_t* wg;
    atomic_int*        marks;
    _churn_task_t*     tasks;
    atomic_int         completed;
    atomic_int         duplicates;
    atomic_int         spawn_errors;
    atomic_int         nested_done;
    atomic_int         producers_ready;
    atomic_int         producers_start;
};

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

static void _churn_task(void* arg) {
    _churn_task_t* task     = (_churn_task_t*)arg;
    _churn_ctx_t*  ctx      = task->ctx;
    int            expected = 0;

    if (!atomic_compare_exchange_strong(&ctx->marks[task->id], &expected, 1)) {
        atomic_fetch_add(&ctx->duplicates, 1);
    }
    atomic_fetch_add(&ctx->completed, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _churn_arrive_start(_churn_ctx_t* ctx) {
    int ready = atomic_fetch_add(&ctx->producers_ready, 1) + 1;

    ASSERT(ready <= SCHED_CHURN_PRODUCERS);
    if (ready == SCHED_CHURN_PRODUCERS) {
        atomic_store(&ctx->producers_start, 1);
    }
}

static void _churn_worker_wait_start(_churn_ctx_t* ctx) {
    _churn_arrive_start(ctx);
    while (!atomic_load(&ctx->producers_start)) {
        runtime_yield();
    }
}

static void _churn_external_wait_start(_churn_ctx_t* ctx) {
    _churn_arrive_start(ctx);
    while (!atomic_load(&ctx->producers_start)) {
        thrd_yield();
    }
}

static int _churn_spawn_range(_churn_range_t* range) {
    int errors = 0;

    for (uint32_t i = range->begin; i < range->end; i++) {
        if (scheduler_coro_spawn(
                runtime_get_scheduler(),
                _churn_task,
                &range->ctx->tasks[i]) != 0) {
            errors++;
            xylem_waitgroup_done(range->ctx->wg);
        }
    }
    return errors;
}

static int _churn_external_thread(void* arg) {
    _churn_range_t* range = (_churn_range_t*)arg;

    _churn_external_wait_start(range->ctx);
    return _churn_spawn_range(range);
}

static void _churn_nested_spawner(void* arg) {
    _churn_range_t* range = (_churn_range_t*)arg;

    _churn_worker_wait_start(range->ctx);
    atomic_fetch_add(&range->ctx->spawn_errors, _churn_spawn_range(range));
    atomic_store(&range->ctx->nested_done, 1);
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

static void test_backend_final_slot_limit(void) {
    scheduler_opts_t opts = {
        .worker_count    = 1,
        .coro_stack_size = 1024U * 1024U,
    };

    scheduler_t* sched = scheduler_create(&opts);
#ifdef MCO_USE_FIBERS
    ASSERT(sched != NULL);
    scheduler_destroy(sched);
#else
    ASSERT(sched == NULL);
#endif
}

static void test_concurrent_spawn_churn(void) {
    _churn_ctx_t ctx = {
        .wg = xylem_waitgroup_create(),
    };
    ASSERT(ctx.wg != NULL);

    ctx.marks = (atomic_int*)calloc(SCHED_CHURN_TASK_COUNT, sizeof(atomic_int));
    ctx.tasks =
        (_churn_task_t*)calloc(SCHED_CHURN_TASK_COUNT, sizeof(_churn_task_t));
    ASSERT(ctx.marks != NULL);
    ASSERT(ctx.tasks != NULL);
    atomic_init(&ctx.completed, 0);
    atomic_init(&ctx.duplicates, 0);
    atomic_init(&ctx.spawn_errors, 0);
    atomic_init(&ctx.nested_done, 0);
    atomic_init(&ctx.producers_ready, 0);
    atomic_init(&ctx.producers_start, 0);

    for (uint32_t i = 0; i < SCHED_CHURN_TASK_COUNT; i++) {
        atomic_init(&ctx.marks[i], 0);
        ctx.tasks[i].ctx = &ctx;
        ctx.tasks[i].id  = i;
    }

    /* The count covers leaf coroutines; the nested spawner is not a leaf. */
    xylem_waitgroup_add(ctx.wg, SCHED_CHURN_TASK_COUNT);
    _churn_range_t direct = {
        .ctx   = &ctx,
        .begin = 0,
        .end   = SCHED_CHURN_DIRECT,
    };
    _churn_range_t external = {
        .ctx   = &ctx,
        .begin = SCHED_CHURN_DIRECT,
        .end   = SCHED_CHURN_DIRECT + SCHED_CHURN_EXTERNAL,
    };
    _churn_range_t nested = {
        .ctx   = &ctx,
        .begin = SCHED_CHURN_DIRECT + SCHED_CHURN_EXTERNAL,
        .end   = SCHED_CHURN_TASK_COUNT,
    };

    ASSERT(
        scheduler_coro_spawn(
            runtime_get_scheduler(),
            _churn_nested_spawner,
            &nested) == 0);
    thrd_t external_thread;
    ASSERT(
        thrd_create(&external_thread, _churn_external_thread, &external) ==
        thrd_success);
    while (atomic_load(&ctx.producers_ready) < SCHED_CHURN_PRODUCERS - 1) {
        runtime_yield();
    }
    ASSERT(atomic_load(&ctx.completed) == 0);
    _churn_worker_wait_start(&ctx);
    atomic_fetch_add(&ctx.spawn_errors, _churn_spawn_range(&direct));

    int external_result = -1;
    ASSERT(thrd_join(external_thread, &external_result) == thrd_success);
    ASSERT(external_result == 0);
    while (!atomic_load(&ctx.nested_done)) {
        runtime_yield();
    }
    xylem_waitgroup_wait(ctx.wg);

    ASSERT(atomic_load(&ctx.spawn_errors) == 0);
    ASSERT(atomic_load(&ctx.duplicates) == 0);
    ASSERT(atomic_load(&ctx.completed) == SCHED_CHURN_TASK_COUNT);
    for (uint32_t i = 0; i < SCHED_CHURN_TASK_COUNT; i++) {
        ASSERT(atomic_load(&ctx.marks[i]) == 1);
    }

    free(ctx.tasks);
    free((void*)ctx.marks);
    xylem_waitgroup_destroy(ctx.wg);
}

static void _test_run_all(void* arg) {
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_spawn_and_exit();
    test_repeated_yield();
    test_reuse_decommitted_stack();
    test_declined_park();
    test_external_ready();
    test_batch_ready();
    if (arg) {
        test_concurrent_spawn_churn();
    }
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_opts_t one_worker   = {.workers = 1};
    xylem_opts_t many_workers = {.workers = 4};

    xylem_run(_test_run_all, NULL, &one_worker);
    xylem_run(_test_run_all, &many_workers, &many_workers);
    test_backend_final_slot_limit();
    return 0;
}
