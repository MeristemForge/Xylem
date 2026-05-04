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

#include "iowait.h"
#include "runtime.h"

#include "minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdlib.h>

enum {
    IOWAIT_IDLE    = 0,
    IOWAIT_WAITING = 1,
    IOWAIT_READY   = 2,
};

struct iowait_s {
    loop_t*               loop;
    platform_poller_sq_t* poller;
    platform_poller_sqe_t sqe;
    loop_timer_t*         rd_timer;
    loop_timer_t*         wr_timer;
    platform_sock_t       fd;
    mco_coro*             rd_coro;
    mco_coro*             wr_coro;
    _Atomic int           rd_state;
    _Atomic int           wr_state;
    bool                  rd_timed_out;
    bool                  wr_timed_out;
    bool                  registered;
    bool                  closed;
};

typedef struct {
    iowait_t* w;
    uint64_t  timeout_ms;
    bool      is_read;
} _iowait_timeout_ctx_t;

/**
 * Try to transition from WAITING to READY and return the coro to
 * schedule. If still IDLE (coro has not yielded yet), mark READY
 * so the coro skips the yield when it gets there.
 */
static mco_coro* _iowait_wake(_Atomic int* state, mco_coro** coro_slot) {
    int expected = IOWAIT_WAITING;
    if (atomic_compare_exchange_strong(state, &expected, IOWAIT_READY)) {
        mco_coro* co = *coro_slot;
        *coro_slot = NULL;
        return co;
    }
    expected = IOWAIT_IDLE;
    atomic_compare_exchange_strong(state, &expected, IOWAIT_READY);
    return NULL;
}

/* Arm the fd on the netpoll. Called from the worker thread directly. */
static void _iowait_arm(iowait_t* w) {
    if (w->closed) {
        return;
    }

    platform_poller_op_t interest = PLATFORM_POLLER_NO_OP;
    if (w->rd_coro) {
        interest |= PLATFORM_POLLER_RD_OP;
    }
    if (w->wr_coro) {
        interest |= PLATFORM_POLLER_WR_OP;
    }
    if (interest == PLATFORM_POLLER_NO_OP) {
        return;
    }

    w->sqe.op = interest;
    if (!w->registered) {
        if (platform_poller_add(w->poller, &w->sqe) == 0) {
            w->registered = true;
        }
    } else {
        platform_poller_mod(w->poller, &w->sqe);
    }
}

static void _iowait_rd_timeout_cb(
    loop_t* loop, loop_timer_t* timer, void* ud) {
    (void)loop;
    (void)timer;
    iowait_t* w = (iowait_t*)ud;
    mco_coro* co = _iowait_wake(&w->rd_state, &w->rd_coro);
    if (co) {
        w->rd_timed_out = true;
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

static void _iowait_wr_timeout_cb(
    loop_t* loop, loop_timer_t* timer, void* ud) {
    (void)loop;
    (void)timer;
    iowait_t* w = (iowait_t*)ud;
    mco_coro* co = _iowait_wake(&w->wr_state, &w->wr_coro);
    if (co) {
        w->wr_timed_out = true;
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

static void _iowait_timeout_start_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)req;
    _iowait_timeout_ctx_t* ctx = (_iowait_timeout_ctx_t*)ud;
    iowait_t* w = ctx->w;

    if (w->closed) {
        free(ctx);
        return;
    }

    if (ctx->is_read) {
        if (!w->rd_timer) {
            w->rd_timer = loop_create_timer(loop);
        }
        loop_start_timer(
            w->rd_timer, _iowait_rd_timeout_cb, w, ctx->timeout_ms, 0);
    } else {
        if (!w->wr_timer) {
            w->wr_timer = loop_create_timer(loop);
        }
        loop_start_timer(
            w->wr_timer, _iowait_wr_timeout_cb, w, ctx->timeout_ms, 0);
    }
    free(ctx);
}

static void _iowait_rd_timeout_stop_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)loop;
    (void)req;
    iowait_t* w = (iowait_t*)ud;
    if (w->rd_timer) {
        loop_stop_timer(w->rd_timer);
    }
}

static void _iowait_wr_timeout_stop_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)loop;
    (void)req;
    iowait_t* w = (iowait_t*)ud;
    if (w->wr_timer) {
        loop_stop_timer(w->wr_timer);
    }
}

iowait_t* iowait_create(loop_t* loop, platform_sock_t fd) {
    iowait_t* w = (iowait_t*)calloc(1, sizeof(iowait_t));
    if (!w) {
        return NULL;
    }

    w->loop = loop;
    w->poller = runtime_get_poller();
    w->fd = fd;

    w->sqe.fd = (platform_poller_fd_t)fd;
    w->sqe.ud = w;
    w->sqe.oneshot = 1;
    w->sqe.op = PLATFORM_POLLER_NO_OP;

    return w;
}

bool iowait_read(iowait_t* w, uint64_t timeout_ms) {
    if (w->closed) {
        return false;
    }

    w->rd_timed_out = false;
    w->rd_coro = mco_running();
    atomic_store(&w->rd_state, IOWAIT_IDLE);

    /* Arm directly on the netpoll -- no cross-thread post needed. */
    _iowait_arm(w);

    /* Timeouts still go through the loop thread (timer heap). */
    if (timeout_ms > 0) {
        _iowait_timeout_ctx_t* ctx =
            (_iowait_timeout_ctx_t*)malloc(sizeof(_iowait_timeout_ctx_t));
        if (ctx) {
            ctx->w = w;
            ctx->timeout_ms = timeout_ms;
            ctx->is_read = true;
            loop_post(w->loop, _iowait_timeout_start_cb, ctx);
        }
    }

    int expected = IOWAIT_IDLE;
    if (atomic_compare_exchange_strong(
            &w->rd_state, &expected, IOWAIT_WAITING)) {
        mco_yield(mco_running());
    }

    atomic_store(&w->rd_state, IOWAIT_IDLE);
    w->rd_coro = NULL;

    if (timeout_ms > 0) {
        loop_post(w->loop, _iowait_rd_timeout_stop_cb, w);
    }

    return !w->rd_timed_out && !w->closed;
}

bool iowait_write(iowait_t* w, uint64_t timeout_ms) {
    if (w->closed) {
        return false;
    }

    w->wr_timed_out = false;
    w->wr_coro = mco_running();
    atomic_store(&w->wr_state, IOWAIT_IDLE);

    _iowait_arm(w);

    if (timeout_ms > 0) {
        _iowait_timeout_ctx_t* ctx =
            (_iowait_timeout_ctx_t*)malloc(sizeof(_iowait_timeout_ctx_t));
        if (ctx) {
            ctx->w = w;
            ctx->timeout_ms = timeout_ms;
            ctx->is_read = false;
            loop_post(w->loop, _iowait_timeout_start_cb, ctx);
        }
    }

    int expected = IOWAIT_IDLE;
    if (atomic_compare_exchange_strong(
            &w->wr_state, &expected, IOWAIT_WAITING)) {
        mco_yield(mco_running());
    }

    atomic_store(&w->wr_state, IOWAIT_IDLE);
    w->wr_coro = NULL;

    if (timeout_ms > 0) {
        loop_post(w->loop, _iowait_wr_timeout_stop_cb, w);
    }

    return !w->wr_timed_out && !w->closed;
}

void iowait_close(iowait_t* w) {
    if (w->closed) {
        return;
    }
    w->closed = true;

    scheduler_t* sched = runtime_get_scheduler();
    if (w->rd_coro) {
        mco_coro* co = _iowait_wake(&w->rd_state, &w->rd_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }
    if (w->wr_coro) {
        mco_coro* co = _iowait_wake(&w->wr_state, &w->wr_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }
}

static void _iowait_destroy_timer_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)loop;
    (void)req;
    iowait_t* w = (iowait_t*)ud;

    if (w->rd_timer) {
        loop_destroy_timer(w->rd_timer);
    }
    if (w->wr_timer) {
        loop_destroy_timer(w->wr_timer);
    }
    free(w);
}

void iowait_destroy(iowait_t* w) {
    if (!w->closed) {
        iowait_close(w);
    }

    if (w->registered) {
        platform_poller_del(w->poller, &w->sqe);
        w->registered = false;
    }

    /* Timers belong to the loop thread -- post their destruction. */
    if (w->rd_timer || w->wr_timer) {
        loop_post(w->loop, _iowait_destroy_timer_cb, w);
    } else {
        free(w);
    }
}

bool iowait_is_closed(iowait_t* w) {
    return w->closed;
}

void iowait_on_event(int revents, void* ud) {
    iowait_t* w = (iowait_t*)ud;
    scheduler_t* sched = runtime_get_scheduler();

    if ((revents & PLATFORM_POLLER_RD_OP)) {
        mco_coro* co = _iowait_wake(&w->rd_state, &w->rd_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }
    if ((revents & PLATFORM_POLLER_WR_OP)) {
        mco_coro* co = _iowait_wake(&w->wr_state, &w->wr_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }

    /* Re-arm for any direction that still has a waiting coro (oneshot). */
    _iowait_arm(w);
}
