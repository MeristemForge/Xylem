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

#define DEADLINE_RACE_THREADS 4
#define DEADLINE_RACE_ITERS   2000
#define WAIT_RACE_ITERS       500
#define TIMEOUT_RACE_ITERS    100
#define EVENT_LOCK_YIELDS     1000

enum {
    TEST_IOWAIT_WAITER_NONE  = 0,
    TEST_IOWAIT_WAITER_WAIT  = 1,
    TEST_IOWAIT_WAITER_READY = 2,
};

typedef struct _test_iowait_dir_s {
    iowait_t*          w;
    _Atomic uintptr_t  waiter;
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

typedef struct {
    iowait_t*        active;
    _Atomic int32_t  requested;
    _Atomic int32_t  completed;
    _Atomic int32_t  released;
    _Atomic int32_t  failures;
    _Atomic bool     finished;
} _wait_race_ctx_t;

typedef struct {
    iowait_t*        active;
    iowait_result_t  result;
    _Atomic bool     started;
    _Atomic bool     finished;
} _single_wait_ctx_t;

typedef struct {
    iowait_t*       active;
    _Atomic bool    started;
    int32_t         batch_count;
} _event_thread_ctx_t;

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

static int _event_thread(void* arg) {
    _event_thread_ctx_t* ctx = (_event_thread_ctx_t*)arg;
    mco_coro* coros[2];
    runnable_batch_t batch = {
        .coros = coros,
        .cap = (int32_t)(sizeof(coros) / sizeof(coros[0])),
        .n = 0,
    };

    atomic_store(&ctx->started, true);
    iowait_on_event(
        runtime_get_scheduler(),
        PLATFORM_POLLER_RD_OP,
        ctx->active->sqe.ud,
        &batch);
    ctx->batch_count = batch.n;
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

static void _iowait_collect_read(iowait_t* w, runnable_batch_t* batch) {
    iowait_on_event(
        runtime_get_scheduler(),
        PLATFORM_POLLER_RD_OP,
        w->sqe.ud,
        batch);
}

static void _iowait_inject_read(iowait_t* w) {
    mco_coro* coros[4];
    runnable_batch_t batch = {
        .coros = coros,
        .cap = (int32_t)(sizeof(coros) / sizeof(coros[0])),
        .n = 0,
    };

    _iowait_collect_read(w, &batch);
    scheduler_schedule_batch(runtime_get_scheduler(), coros, batch.n);
}

static void _iowait_open(_iowait_ctx_t* ctx) {
    ASSERT(platform_socket_socketpair(AF_INET, SOCK_STREAM, 0, ctx->socks)
           == 0);
    platform_socket_enable_nonblocking(ctx->socks[0], true);
    platform_socket_enable_nonblocking(ctx->socks[1], true);
    ctx->active = iowait_create(ctx->socks[0]);
    ASSERT(ctx->active != NULL);
}

static void _iowait_dispose(_iowait_ctx_t* ctx) {
    iowait_destroy(ctx->active);
    platform_socket_close(ctx->socks[0]);
    platform_socket_close(ctx->socks[1]);
}

static void _wait_race_coro(void* arg) {
    _wait_race_ctx_t* ctx = (_wait_race_ctx_t*)arg;
    for (int32_t i = 1; i <= WAIT_RACE_ITERS; i++) {
        atomic_store(&ctx->requested, i);
        if (iowait_read(ctx->active) != IOWAIT_READY) {
            atomic_fetch_add(&ctx->failures, 1);
        }
        atomic_store(&ctx->completed, i);
        while (atomic_load(&ctx->released) < i) {
            runtime_yield();
        }
    }
    atomic_store(&ctx->finished, true);
}

static void _timeout_race_coro(void* arg) {
    _wait_race_ctx_t* ctx = (_wait_race_ctx_t*)arg;
    for (int32_t i = 1; i <= TIMEOUT_RACE_ITERS; i++) {
        atomic_store(&ctx->requested, i);
        if (iowait_read(ctx->active) != IOWAIT_TIMEOUT) {
            atomic_fetch_add(&ctx->failures, 1);
        }
        atomic_store(&ctx->completed, i);
        while (atomic_load(&ctx->released) < i) {
            runtime_yield();
        }
    }
    atomic_store(&ctx->finished, true);
}

static void _single_wait_coro(void* arg) {
    _single_wait_ctx_t* ctx = (_single_wait_ctx_t*)arg;
    atomic_store(&ctx->started, true);
    ctx->result = iowait_read(ctx->active);
    atomic_store(&ctx->finished, true);
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
    _single_wait_ctx_t waiter = {.result = IOWAIT_ERROR};

    _iowait_open(ctx);
    waiter.active = ctx->active;

    xylem_spawn(_single_wait_coro, &waiter);
    while (atomic_load(&ctx->active->rd.waiter)
           <= TEST_IOWAIT_WAITER_READY) {
        runtime_yield();
    }

    iowait_close(ctx->active);
    _iowait_inject_read(ctx->active);
    while (!atomic_load(&waiter.finished)) {
        runtime_yield();
    }

    ASSERT(waiter.result == IOWAIT_CLOSED);
    ctx->tested = 1;

    _iowait_dispose(ctx);
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

static void test_readiness_is_retained_before_wait(void) {
    _iowait_ctx_t ctx = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    mco_coro* coros[2];
    runnable_batch_t batch = {
        .coros = coros,
        .cap = (int32_t)(sizeof(coros) / sizeof(coros[0])),
        .n = 0,
    };

    _iowait_open(&ctx);
    _iowait_collect_read(ctx.active, &batch);
    ASSERT(batch.n == 0);
    ASSERT(atomic_load(&ctx.active->rd.waiter)
           == TEST_IOWAIT_WAITER_READY);
    ASSERT(iowait_read(ctx.active) == IOWAIT_READY);
    ASSERT(atomic_load(&ctx.active->rd.waiter)
           == TEST_IOWAIT_WAITER_NONE);

    iowait_close(ctx.active);
    _iowait_dispose(&ctx);
}

static void test_readiness_resolves_wait_reservation(void) {
    _iowait_ctx_t ctx = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    mco_coro* coros[2];
    runnable_batch_t batch = {
        .coros = coros,
        .cap = (int32_t)(sizeof(coros) / sizeof(coros[0])),
        .n = 0,
    };

    _iowait_open(&ctx);
    atomic_store(&ctx.active->rd.waiter, TEST_IOWAIT_WAITER_WAIT);
    _iowait_collect_read(ctx.active, &batch);
    ASSERT(batch.n == 0);
    ASSERT(atomic_load(&ctx.active->rd.waiter)
           == TEST_IOWAIT_WAITER_READY);

    iowait_close(ctx.active);
    _iowait_dispose(&ctx);
}

static void test_close_cancels_wait_reservation(void) {
    _iowait_ctx_t ctx = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };

    _iowait_open(&ctx);
    atomic_store(&ctx.active->rd.waiter, TEST_IOWAIT_WAITER_WAIT);
    iowait_close(ctx.active);
    ASSERT(atomic_load(&ctx.active->rd.waiter)
           == TEST_IOWAIT_WAITER_NONE);
    _iowait_dispose(&ctx);
}

static void test_readiness_races_wait_publication(void) {
    _iowait_ctx_t owner = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    _wait_race_ctx_t ctx = {0};

    _iowait_open(&owner);
    ctx.active = owner.active;
    xylem_spawn(_wait_race_coro, &ctx);

    for (int32_t i = 1; i <= WAIT_RACE_ITERS; i++) {
        while (atomic_load(&ctx.requested) < i) {
            runtime_yield();
        }
        _iowait_inject_read(ctx.active);
        while (atomic_load(&ctx.completed) < i) {
            runtime_yield();
        }
        atomic_store(&ctx.released, i);
    }
    while (!atomic_load(&ctx.finished)) {
        runtime_yield();
    }

    ASSERT(atomic_load(&ctx.failures) == 0);
    iowait_close(owner.active);
    _iowait_dispose(&owner);
}

static void test_close_races_wait(void) {
    _iowait_ctx_t owner = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    _single_wait_ctx_t ctx = {.result = IOWAIT_ERROR};

    _iowait_open(&owner);
    ctx.active = owner.active;
    xylem_spawn(_single_wait_coro, &ctx);
    while (!atomic_load(&ctx.started)) {
        runtime_yield();
    }
    iowait_close(ctx.active);
    while (!atomic_load(&ctx.finished)) {
        runtime_yield();
    }

    ASSERT(ctx.result == IOWAIT_CLOSED);
    _iowait_dispose(&owner);
}

static void test_timeout_races_wait(void) {
    _iowait_ctx_t owner = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    _wait_race_ctx_t ctx = {0};

    _iowait_open(&owner);
    ctx.active = owner.active;
    xylem_spawn(_timeout_race_coro, &ctx);

    for (int32_t i = 1; i <= TIMEOUT_RACE_ITERS; i++) {
        while (atomic_load(&ctx.requested) < i) {
            runtime_yield();
        }
        iowait_set_rd_deadline(
            ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
        while (atomic_load(&ctx.completed) < i) {
            runtime_yield();
        }
        iowait_set_rd_deadline(ctx.active, 0);
        atomic_store(&ctx.released, i);
    }
    while (!atomic_load(&ctx.finished)) {
        runtime_yield();
    }

    ASSERT(atomic_load(&ctx.failures) == 0);
    iowait_close(owner.active);
    _iowait_dispose(&owner);
}

static void test_ready_wins_over_close(void) {
    _iowait_ctx_t ctx = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };

    _iowait_open(&ctx);
    _iowait_inject_read(ctx.active);
    iowait_close(ctx.active);
    ASSERT(iowait_read(ctx.active) == IOWAIT_READY);
    _iowait_dispose(&ctx);
}

static void test_closed_state_blocks_stale_read_publication(void) {
    _iowait_ctx_t owner = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    _event_thread_ctx_t event = {0};
    thrd_t thread;

    _iowait_open(&owner);
    event.active = owner.active;

    mtx_lock(&owner.active->arm_lock);
    atomic_store(&owner.active->rd.waiter, TEST_IOWAIT_WAITER_NONE);
    atomic_store(&owner.active->closed, false);
    ASSERT(thrd_create(&thread, _event_thread, &event) == thrd_success);
    while (!atomic_load(&event.started)) {
        thrd_yield();
    }
    for (int32_t i = 0;
         i < EVENT_LOCK_YIELDS
         && atomic_load(&owner.active->rd.waiter)
                == TEST_IOWAIT_WAITER_NONE;
         i++) {
        thrd_yield();
    }
    atomic_store(&owner.active->closed, true);
    mtx_unlock(&owner.active->arm_lock);

    ASSERT(thrd_join(thread, NULL) == thrd_success);
    ASSERT(atomic_load(&owner.active->rd.waiter)
           == TEST_IOWAIT_WAITER_NONE);
    ASSERT(event.batch_count == 0);

    atomic_store(&owner.active->closed, false);
    iowait_close(owner.active);
    _iowait_dispose(&owner);
}

static void test_timeout_wins_over_internal_error(void) {
    _iowait_ctx_t ctx = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };

    _iowait_open(&ctx);
    atomic_store(
        &ctx.active->rd.deadline,
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    atomic_store(&ctx.active->rd.deadline_error, true);
    ASSERT(iowait_read(ctx.active) == IOWAIT_TIMEOUT);
    atomic_store(&ctx.active->rd.deadline, 0);
    atomic_store(&ctx.active->rd.deadline_error, false);
    iowait_close(ctx.active);
    _iowait_dispose(&ctx);
}

static void test_ready_batch_has_no_duplicate_coroutine(void) {
    _iowait_ctx_t owner = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    _single_wait_ctx_t ctx = {.result = IOWAIT_ERROR};
    mco_coro* coros[4];
    runnable_batch_t batch = {
        .coros = coros,
        .cap = (int32_t)(sizeof(coros) / sizeof(coros[0])),
        .n = 0,
    };

    _iowait_open(&owner);
    ctx.active = owner.active;
    xylem_spawn(_single_wait_coro, &ctx);
    while (atomic_load(&owner.active->rd.waiter)
           <= TEST_IOWAIT_WAITER_READY) {
        runtime_yield();
    }

    _iowait_collect_read(owner.active, &batch);
    _iowait_collect_read(owner.active, &batch);
    ASSERT(batch.n == 1);
    scheduler_schedule_batch(runtime_get_scheduler(), batch.coros, batch.n);
    while (!atomic_load(&ctx.finished)) {
        runtime_yield();
    }
    ASSERT(ctx.result == IOWAIT_READY);

    iowait_close(owner.active);
    _iowait_dispose(&owner);
}

static void test_stale_event_after_generation_wrap_is_rejected(void) {
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

    test_readiness_is_retained_before_wait();
    test_readiness_resolves_wait_reservation();
    test_close_cancels_wait_reservation();
    test_readiness_races_wait_publication();
    test_close_races_wait();
    test_timeout_races_wait();
    test_ready_wins_over_close();
    test_closed_state_blocks_stale_read_publication();
    test_timeout_wins_over_internal_error();
    test_ready_batch_has_no_duplicate_coroutine();
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
