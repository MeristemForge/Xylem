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

enum {
    TEST_IOWAIT_INFO_CLOSED     = 1u << 0,
    TEST_IOWAIT_INFO_RD_TIMEOUT = 1u << 1,
    TEST_IOWAIT_INFO_WR_TIMEOUT = 1u << 2,
    TEST_IOWAIT_INFO_RD_ERROR   = 1u << 3,
    TEST_IOWAIT_INFO_WR_ERROR   = 1u << 4,
};

typedef struct _test_iowait_dir_s {
    iowait_t*          w;
    _Atomic uintptr_t  waiter;
    scheduler_timer_t* timer;
    mtx_t              deadline_lock;
    _Atomic uint64_t   deadline;
} _test_iowait_dir_t;

struct iowait_s {
    platform_poller_sq_t* poller;
    platform_poller_sqe_t sqe;
    platform_sock_t       fd;

    _test_iowait_dir_t    rd;
    _test_iowait_dir_t    wr;

    mtx_t                 poll_lock;

    _Atomic int32_t       refcnt;
    _Atomic uint16_t      gen;
    _Atomic int           interest;
    _Atomic uint32_t      poll_info;

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
    iowait_t*    active;
    _Atomic bool started;
    size_t       runnable_count;
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
    mco_coro* runnables[IOWAIT_EVENT_RUNNABLE_CAP];

    atomic_store(&ctx->started, true);
    ctx->runnable_count = iowait_process_event(
        runtime_get_scheduler(),
        PLATFORM_POLLER_RD_OP,
        ctx->active->sqe.ud,
        runnables);
    return 0;
}

static void _iowait_inject_stale_read(void* ud) {
    mco_coro* runnables[IOWAIT_EVENT_RUNNABLE_CAP];
    size_t runnable_count = iowait_process_event(
        runtime_get_scheduler(),
        PLATFORM_POLLER_RD_OP,
        ud,
        runnables);
    scheduler_coro_ready_batch(
        runtime_get_scheduler(),
        runnables,
        (int)runnable_count);
}

static size_t _iowait_collect_read(
    iowait_t*  w,
    mco_coro** runnables) {
    return iowait_process_event(
        runtime_get_scheduler(),
        PLATFORM_POLLER_RD_OP,
        w->sqe.ud,
        runnables);
}

static void _iowait_inject_read(iowait_t* w) {
    mco_coro* runnables[IOWAIT_EVENT_RUNNABLE_CAP];
    size_t runnable_count = _iowait_collect_read(w, runnables);
    scheduler_coro_ready_batch(
        runtime_get_scheduler(),
        runnables,
        (int)runnable_count);
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

static void _single_write_wait_coro(void* arg) {
    _single_wait_ctx_t* ctx = (_single_wait_ctx_t*)arg;
    atomic_store(&ctx->started, true);
    ctx->result = iowait_write(ctx->active);
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
    mco_coro* runnables[IOWAIT_EVENT_RUNNABLE_CAP];

    _iowait_open(&ctx);
    size_t runnable_count = _iowait_collect_read(ctx.active, runnables);
    ASSERT(runnable_count == 0);
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
    mco_coro* runnables[IOWAIT_EVENT_RUNNABLE_CAP];

    _iowait_open(&ctx);
    atomic_store(&ctx.active->rd.waiter, TEST_IOWAIT_WAITER_WAIT);
    size_t runnable_count = _iowait_collect_read(ctx.active, runnables);
    ASSERT(runnable_count == 0);
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

    mtx_lock(&owner.active->poll_lock);
    atomic_store(&owner.active->rd.waiter, TEST_IOWAIT_WAITER_NONE);
    atomic_fetch_and(
        &owner.active->poll_info, ~TEST_IOWAIT_INFO_CLOSED);
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
    atomic_fetch_or(&owner.active->poll_info, TEST_IOWAIT_INFO_CLOSED);
    mtx_unlock(&owner.active->poll_lock);

    ASSERT(thrd_join(thread, NULL) == thrd_success);
    ASSERT(atomic_load(&owner.active->rd.waiter)
           == TEST_IOWAIT_WAITER_NONE);
    ASSERT(event.runnable_count == 0);

    atomic_fetch_and(
        &owner.active->poll_info, ~TEST_IOWAIT_INFO_CLOSED);
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
    atomic_fetch_or(
        &ctx.active->poll_info,
        TEST_IOWAIT_INFO_RD_TIMEOUT | TEST_IOWAIT_INFO_RD_ERROR);
    ASSERT(iowait_read(ctx.active) == IOWAIT_TIMEOUT);
    atomic_fetch_and(
        &ctx.active->poll_info,
        ~(TEST_IOWAIT_INFO_RD_TIMEOUT | TEST_IOWAIT_INFO_RD_ERROR));
    iowait_close(ctx.active);
    _iowait_dispose(&ctx);
}

static void test_timeout_updates_poll_info(void) {
    _iowait_ctx_t ctx = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };

    _iowait_open(&ctx);
    iowait_set_rd_deadline(
        ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    ASSERT(iowait_read_deadline_expired(ctx.active));
    ASSERT(atomic_load(&ctx.active->poll_info)
           & TEST_IOWAIT_INFO_RD_TIMEOUT);

    iowait_set_rd_deadline(ctx.active, 0);
    ASSERT(!(atomic_load(&ctx.active->poll_info)
             & TEST_IOWAIT_INFO_RD_TIMEOUT));
    iowait_close(ctx.active);
    _iowait_dispose(&ctx);
}

static void test_poll_info_isolates_directions(void) {
    _iowait_ctx_t ctx = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    uint32_t info;

    _iowait_open(&ctx);
    atomic_fetch_or(&ctx.active->poll_info, TEST_IOWAIT_INFO_WR_ERROR);

    iowait_set_rd_deadline(
        ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    info = atomic_load(&ctx.active->poll_info);
    ASSERT(info & TEST_IOWAIT_INFO_RD_TIMEOUT);
    ASSERT(info & TEST_IOWAIT_INFO_WR_ERROR);

    iowait_set_rd_deadline(ctx.active, 0);
    info = atomic_load(&ctx.active->poll_info);
    ASSERT(!(info & (TEST_IOWAIT_INFO_RD_TIMEOUT
                     | TEST_IOWAIT_INFO_RD_ERROR)));
    ASSERT(info & TEST_IOWAIT_INFO_WR_ERROR);

    iowait_set_wr_deadline(
        ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    info = atomic_load(&ctx.active->poll_info);
    ASSERT(info & TEST_IOWAIT_INFO_WR_TIMEOUT);
    ASSERT(!(info & TEST_IOWAIT_INFO_WR_ERROR));

    iowait_close(ctx.active);
    _iowait_dispose(&ctx);
}

static void test_event_runnables_have_no_duplicate_coroutine(void) {
    _iowait_ctx_t owner = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    _single_wait_ctx_t ctx = {.result = IOWAIT_ERROR};
    mco_coro* runnables[IOWAIT_EVENT_RUNNABLE_CAP];
    mco_coro* duplicates[IOWAIT_EVENT_RUNNABLE_CAP];

    _iowait_open(&owner);
    ctx.active = owner.active;
    xylem_spawn(_single_wait_coro, &ctx);
    while (atomic_load(&owner.active->rd.waiter)
           <= TEST_IOWAIT_WAITER_READY) {
        runtime_yield();
    }

    size_t runnable_count = _iowait_collect_read(owner.active, runnables);
    size_t duplicate_count = _iowait_collect_read(owner.active, duplicates);
    ASSERT(runnable_count == 1);
    ASSERT(duplicate_count == 0);
    scheduler_coro_ready_batch(
        runtime_get_scheduler(),
        runnables,
        (int)runnable_count);
    while (!atomic_load(&ctx.finished)) {
        runtime_yield();
    }
    ASSERT(ctx.result == IOWAIT_READY);

    iowait_close(owner.active);
    _iowait_dispose(&owner);
}

static void test_event_returns_both_direction_runnables(void) {
    _iowait_ctx_t owner = {
        .socks = {
            PLATFORM_SO_ERROR_INVALID_SOCKET,
            PLATFORM_SO_ERROR_INVALID_SOCKET,
        },
    };
    _single_wait_ctx_t rd = {.result = IOWAIT_ERROR};
    _single_wait_ctx_t wr = {.result = IOWAIT_ERROR};
    mco_coro* runnables[IOWAIT_EVENT_RUNNABLE_CAP];

    ASSERT(platform_socket_socketpair(
               AF_INET, SOCK_STREAM, 0, owner.socks)
           == 0);
    platform_socket_enable_nonblocking(owner.socks[0], true);
    platform_socket_enable_nonblocking(owner.socks[1], true);
    platform_socket_set_sndbuf(owner.socks[0], 4096);
    platform_socket_set_rcvbuf(owner.socks[1], 4096);

    char fill[4096] = {0};
    for (;;) {
        ssize_t n = platform_socket_send(owner.socks[0], fill, sizeof(fill));
        if (n > 0) {
            continue;
        }
        ASSERT(n == -1);
        int err = platform_socket_get_lasterror();
        ASSERT(err == PLATFORM_SO_ERROR_EAGAIN
               || err == PLATFORM_SO_ERROR_EWOULDBLOCK);
        break;
    }

    owner.active = iowait_create(owner.socks[0]);
    ASSERT(owner.active != NULL);
    rd.active = owner.active;
    wr.active = owner.active;
    xylem_spawn(_single_wait_coro, &rd);
    xylem_spawn(_single_write_wait_coro, &wr);
    for (int i = 0; i < EVENT_LOCK_YIELDS; i++) {
        runtime_yield();
    }
    ASSERT(atomic_load(&owner.active->rd.waiter)
           > TEST_IOWAIT_WAITER_READY);
    ASSERT(atomic_load(&owner.active->wr.waiter)
           > TEST_IOWAIT_WAITER_READY);

    size_t runnable_count = iowait_process_event(
        runtime_get_scheduler(),
        PLATFORM_POLLER_RD_OP | PLATFORM_POLLER_WR_OP,
        owner.active->sqe.ud,
        runnables);
    ASSERT(runnable_count == IOWAIT_EVENT_RUNNABLE_CAP);
    ASSERT(runnables[0] != runnables[1]);
    scheduler_coro_ready_batch(
        runtime_get_scheduler(),
        runnables,
        (int)runnable_count);
    while (!atomic_load(&rd.finished) || !atomic_load(&wr.finished)) {
        runtime_yield();
    }
    ASSERT(rd.result == IOWAIT_READY);
    ASSERT(wr.result == IOWAIT_READY);

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
    test_timeout_updates_poll_info();
    test_poll_info_isolates_directions();
    test_event_runnables_have_no_duplicate_coroutine();
    test_event_returns_both_direction_runnables();
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
