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

#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"
#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define DEADLINE_RACE_THREADS 4
#define DEADLINE_RACE_ITERS   2000

typedef struct _test_iowait_dir_s {
    iowait_t*          w;
    _Atomic uintptr_t  state;
    scheduler_timer_t* timer;
    mtx_t              deadline_lock;
    _Atomic uint64_t   deadline;
    _Atomic bool       deadline_error;
} _test_iowait_dir_t;

struct iowait_s {
    platform_poller_sq_t* poller;
    platform_poller_sqe_t sqe;
    platform_sock_t       fd;

    _test_iowait_dir_t    rd;
    _test_iowait_dir_t    wr;

    mtx_t                 arm_lock;

    _Atomic int32_t       refcnt;
    _Atomic uint16_t      gen;
    _Atomic int           interest;
    _Atomic bool          closed;

    iowait_slab_t*        slab;
    uint32_t              slot_index;
};

typedef struct {
    platform_sock_t socks[2];
    iowait_t*       active;
    void*           stale_ud;
    iowait_result_t result;
    int             tested;
} _iowait_ctx_t;

typedef struct {
    platform_sock_t socks[2];
    iowait_t*       active;
    _Atomic bool    start;
    int             tested;
} _deadline_race_ctx_t;

static void _iowait_wait_coro(void* arg) {
    _iowait_ctx_t* ctx = (_iowait_ctx_t*)arg;
    ctx->result = iowait_read(ctx->active);
}

static int _deadline_setter_thread(void* arg) {
    _deadline_race_ctx_t* ctx = (_deadline_race_ctx_t*)arg;
    while (!atomic_load(&ctx->start)) {
        thrd_yield();
    }

    for (int i = 0; i < DEADLINE_RACE_ITERS; i++) {
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        iowait_set_rd_deadline(ctx->active, now + 1000 + (uint64_t)(i & 7));
        if ((i & 3) == 0) {
            iowait_set_rd_deadline(ctx->active, 0);
        }
    }
    return 0;
}

static void _iowait_inject_stale_read(void* ud) {
    mco_coro* coros[4];
    runnable_batch_t batch = {
        .coros = coros,
        .cap = (int32_t)(sizeof(coros) / sizeof(coros[0])),
        .n = 0,
    };

    iowait_on_event(
        runtime_get_scheduler(),
        PLATFORM_POLLER_RD_OP,
        ud,
        &batch);
    scheduler_schedule_batch(runtime_get_scheduler(), coros, batch.n);
}

static void _iowait_inject_read(iowait_t* w) {
    mco_coro* coros[4];
    runnable_batch_t batch = {
        .coros = coros,
        .cap = (int32_t)(sizeof(coros) / sizeof(coros[0])),
        .n = 0,
    };

    iowait_on_event(
        runtime_get_scheduler(),
        PLATFORM_POLLER_RD_OP,
        w->sqe.ud,
        &batch);
    scheduler_schedule_batch(runtime_get_scheduler(), coros, batch.n);
}

static void _iowait_wrap_coro(void* arg) {
    _iowait_ctx_t* ctx = (_iowait_ctx_t*)arg;

    ASSERT(platform_socket_socketpair(AF_INET, SOCK_STREAM, 0, ctx->socks)
           == 0);
    platform_socket_enable_nonblocking(ctx->socks[0], true);
    platform_socket_enable_nonblocking(ctx->socks[1], true);

    iowait_t* stale = iowait_create(ctx->socks[0]);
    ASSERT(stale != NULL);
    ctx->stale_ud = stale->sqe.ud;
    iowait_destroy(stale);

    ctx->active = iowait_create(ctx->socks[0]);
    ASSERT(ctx->active != NULL);

    xylem_spawn(_iowait_wait_coro, ctx);
    xylem_sleep(1);
    _iowait_inject_stale_read(ctx->stale_ud);
    xylem_sleep(1);
    iowait_close(ctx->active);
    xylem_sleep(1);

    ASSERT(ctx->result == IOWAIT_CLOSED);
    ctx->tested = 1;

    iowait_destroy(ctx->active);
    platform_socket_close(ctx->socks[0]);
    platform_socket_close(ctx->socks[1]);
}

static void _iowait_closed_before_event_coro(void* arg) {
    _iowait_ctx_t* ctx = (_iowait_ctx_t*)arg;

    ASSERT(platform_socket_socketpair(AF_INET, SOCK_STREAM, 0, ctx->socks)
           == 0);
    platform_socket_enable_nonblocking(ctx->socks[0], true);
    platform_socket_enable_nonblocking(ctx->socks[1], true);

    ctx->active = iowait_create(ctx->socks[0]);
    ASSERT(ctx->active != NULL);

    xylem_spawn(_iowait_wait_coro, ctx);
    xylem_sleep(1);

    atomic_store(&ctx->active->closed, true);
    _iowait_inject_read(ctx->active);
    xylem_sleep(1);

    ASSERT(ctx->result == IOWAIT_CLOSED);
    ctx->tested = 1;

    atomic_store(&ctx->active->closed, false);
    iowait_close(ctx->active);
    iowait_destroy(ctx->active);
    platform_socket_close(ctx->socks[0]);
    platform_socket_close(ctx->socks[1]);
}

static void _iowait_deadline_race_coro(void* arg) {
    _deadline_race_ctx_t* ctx = (_deadline_race_ctx_t*)arg;
    thrd_t                threads[DEADLINE_RACE_THREADS];

    ASSERT(platform_socket_socketpair(AF_INET, SOCK_STREAM, 0, ctx->socks)
           == 0);
    platform_socket_enable_nonblocking(ctx->socks[0], true);
    platform_socket_enable_nonblocking(ctx->socks[1], true);

    ctx->active = iowait_create(ctx->socks[0]);
    ASSERT(ctx->active != NULL);

    for (int i = 0; i < DEADLINE_RACE_THREADS; i++) {
        ASSERT(thrd_create(&threads[i], _deadline_setter_thread, ctx)
               == thrd_success);
    }

    atomic_store(&ctx->start, true);
    xylem_sleep(1);
    iowait_close(ctx->active);

    for (int i = 0; i < DEADLINE_RACE_THREADS; i++) {
        ASSERT(thrd_join(threads[i], NULL) == thrd_success);
    }

    ctx->tested = 1;
    iowait_destroy(ctx->active);
    platform_socket_close(ctx->socks[0]);
    platform_socket_close(ctx->socks[1]);
}

static void test_stale_event_after_generation_wrap_is_rejected(void) {
    fprintf(stderr, "=== test_stale_event_after_generation_wrap_is_rejected\n");
    _iowait_ctx_t ctx = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
        .result = IOWAIT_ERROR,
    };
    _iowait_wrap_coro(&ctx);
    ASSERT(ctx.tested == 1);
}

static void test_closed_state_wins_over_late_event(void) {
    fprintf(stderr, "=== test_closed_state_wins_over_late_event\n");
    _iowait_ctx_t ctx = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
        .result = IOWAIT_ERROR,
    };
    _iowait_closed_before_event_coro(&ctx);
    ASSERT(ctx.tested == 1);
}

static void test_concurrent_deadline_setters_and_close(void) {
    fprintf(stderr, "=== test_concurrent_deadline_setters_and_close\n");
    _deadline_race_ctx_t ctx = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    atomic_init(&ctx.start, false);
    _iowait_deadline_race_coro(&ctx);
    ASSERT(ctx.tested == 1);
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_stale_event_after_generation_wrap_is_rejected();
    test_closed_state_wins_over_late_event();
    test_concurrent_deadline_setters_and_close();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, NULL);
    return 0;
}
