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
#include "xylem/xylem-threads.h"
#include "assert.h"
#include "utils.h"

#include "runtime/scheduler.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define STACK_FRAME_BYTES 8192
#define STACK_DEPTH       8
#define STACK_TASK_COUNT  32
#define STACK_REUSE_ROUNDS 128
#define YIELD_ROUNDS      32

typedef struct {
    _Atomic uint32_t arrived;
    atomic_bool      released;
} _barrier_t;

typedef struct {
    _barrier_t* barrier;
    uint64_t    checksum;
    uint32_t    depth;
    uint8_t     seed;
    atomic_bool done;
} _stack_ctx_t;

typedef struct {
    xylem_sem_t* requested;
    xylem_sem_t* notified;
} _notify_ctx_t;

typedef struct {
    uint32_t completed;
} _suite_ctx_t;

static void _fill_frame(volatile uint8_t* frame, uint8_t value) {
    for (size_t i = 0; i < STACK_FRAME_BYTES; i++) {
        frame[i] = value;
    }
}

static uint64_t _check_frame(const volatile uint8_t* frame, uint8_t expected) {
    uint64_t checksum = 0;
    for (size_t i = 0; i < STACK_FRAME_BYTES; i++) {
        ASSERT(frame[i] == expected);
        checksum += frame[i];
    }
    return checksum;
}

static uint64_t _stack_recurse(uint32_t depth, uint8_t seed, _barrier_t* barrier) {
    volatile uint8_t frame[STACK_FRAME_BYTES];
    uint8_t value = (uint8_t)(seed + depth);
    _fill_frame(frame, value);
    uint64_t checksum = 0;
    if (depth > 0) {
        checksum = _stack_recurse(depth - 1, seed, barrier);
    } else if (barrier) {
        atomic_fetch_add(&barrier->arrived, 1);
        while (!atomic_load(&barrier->released)) {
            scheduler_coro_yield();
        }
    }
    scheduler_coro_yield();
    return checksum + _check_frame(frame, value);
}

static uint64_t _expected_checksum(uint32_t depth, uint8_t seed) {
    uint64_t levels = (uint64_t)depth + 1;
    return STACK_FRAME_BYTES * levels * (2 * (uint64_t)seed + depth) / 2;
}

static void _stack_task(void* arg) {
    _stack_ctx_t* ctx = (_stack_ctx_t*)arg;
    ctx->checksum = _stack_recurse(ctx->depth, ctx->seed, ctx->barrier);
    atomic_store(&ctx->done, true);
}

static void _wait_stack_task(_stack_ctx_t* ctx) {
    while (!atomic_load(&ctx->done)) {
        scheduler_coro_yield();
    }
    ASSERT(ctx->checksum == _expected_checksum(ctx->depth, ctx->seed));
}

static int _notifier_thread(void* arg) {
    _notify_ctx_t* ctx = (_notify_ctx_t*)arg;
    for (int i = 0; i < YIELD_ROUNDS; i++) {
        xylem_sem_wait(ctx->requested);
        xylem_sem_post(ctx->notified);
    }
    return 0;
}

static void test_yield_preserves_locals(void) {
    volatile uint8_t frame[STACK_FRAME_BYTES];
    uint32_t value = 0;
    _fill_frame(frame, 0x5a);
    for (uint32_t i = 0; i < YIELD_ROUNDS; i++) {
        value++;
        scheduler_coro_yield();
        ASSERT(value == i + 1);
        ASSERT(_check_frame(frame, 0x5a) == (uint64_t)STACK_FRAME_BYTES * 0x5a);
    }
}

static void test_nested_spawn_preserves_parent_stack(void) {
    volatile uint8_t frame[STACK_FRAME_BYTES];
    _fill_frame(frame, 0xa5);
    _stack_ctx_t child = {.depth = STACK_DEPTH, .seed = 0x21};
    xylem_spawn(_stack_task, &child);
    _wait_stack_task(&child);
    ASSERT(_check_frame(frame, 0xa5) == (uint64_t)STACK_FRAME_BYTES * 0xa5);
}

static void test_repeated_coroutines_preserve_stack_data(void) {
    for (uint32_t i = 0; i < STACK_REUSE_ROUNDS; i++) {
        _stack_ctx_t ctx = {
            .depth = i % 2 == 0 ? STACK_DEPTH : 0,
            .seed = (uint8_t)(0x20 + i % 32),
        };
        xylem_spawn(_stack_task, &ctx);
        _wait_stack_task(&ctx);
    }
}

static void test_concurrent_stacks_are_independent(void) {
    _barrier_t barrier = {0};
    _stack_ctx_t tasks[STACK_TASK_COUNT];
    for (uint32_t i = 0; i < STACK_TASK_COUNT; i++) {
        tasks[i] = (_stack_ctx_t){
            .barrier = &barrier,
            .depth = STACK_DEPTH,
            .seed = (uint8_t)(0x20 + i),
        };
        xylem_spawn(_stack_task, &tasks[i]);
    }
    /* Release only after every coroutine has all recursive frames live. */
    while (atomic_load(&barrier.arrived) != STACK_TASK_COUNT) {
        scheduler_coro_yield();
    }
    atomic_store(&barrier.released, true);
    for (size_t i = 0; i < STACK_TASK_COUNT; i++) {
        _wait_stack_task(&tasks[i]);
    }
}

static void test_thread_notifications_preserve_stack(void) {
    volatile uint8_t frame[STACK_FRAME_BYTES];
    _fill_frame(frame, 0x3c);
    _notify_ctx_t ctx = {
        .requested = xylem_sem_create(0),
        .notified = xylem_sem_create(0),
    };
    ASSERT(ctx.requested != NULL && ctx.notified != NULL);
    thrd_t thread;
    ASSERT(thrd_create(&thread, _notifier_thread, &ctx) == thrd_success);
    for (int i = 0; i < YIELD_ROUNDS; i++) {
        xylem_sem_post(ctx.requested);
        xylem_sem_wait(ctx.notified);
        ASSERT(_check_frame(frame, 0x3c) == (uint64_t)STACK_FRAME_BYTES * 0x3c);
    }
    int result = -1;
    ASSERT(thrd_join(thread, &result) == thrd_success);
    ASSERT(result == 0);
    xylem_sem_destroy(ctx.requested);
    xylem_sem_destroy(ctx.notified);
}

static void _test_run_all(void* arg) {
    _suite_ctx_t* ctx = (_suite_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    test_yield_preserves_locals();
    test_nested_spawn_preserves_parent_stack();
    test_repeated_coroutines_preserve_stack_data();
    test_concurrent_stacks_are_independent();
    test_thread_notifications_preserve_stack();
    _utils_watchdog_stop();
    ctx->completed++;
}

int main(void) {
    _suite_ctx_t ctx = {0};
    xylem_opts_t opts = {.workers = 1};
    xylem_run(_test_run_all, &ctx, &opts);
    ASSERT(ctx.completed == 1);
    opts.workers = 4;
    xylem_run(_test_run_all, &ctx, &opts);
    ASSERT(ctx.completed == 2);
    return 0;
}
